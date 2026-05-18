/**
 * @file main.cpp
 * @brief Точка входа приложения Tomorrow Bot
 * 
 * Последовательность инициализации:
 * 1. Парсинг аргументов командной строки
 * 2. Загрузка и валидация конфигурации
 * 3. Инициализация компонентов
 * 4. Сканирование торговых пар (ScannerEngine)
 * 5. Создание pipeline для каждого выбранного символа
 * 6. Создание и запуск супервизора
 * 7. Запуск ротации пар (каждые 24ч)
 * 8. Ожидание сигнала завершения
 * 9. Корректное завершение работы
 */
#include "app_bootstrap.hpp"
#include "health_state.hpp"
#include "http_server.hpp"
#include "runtime_manifest.hpp"
#include "supervisor/supervisor.hpp"
#include "pipeline/trading_pipeline.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "scanner/scanner_engine.hpp"
#include "common/constants.hpp"
#include "common/exchange_rules.hpp"
#include "exchange/bitget/bitget_futures_order_submitter.hpp"
#include "exchange/bitget/bitget_futures_query_adapter.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "security/secret_provider.hpp"
#include "common/enums.hpp"
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <exception>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

/// Глобальный флаг раннего shutdown (до того как `Supervisor` зарегистрирует свой handler).
/// Устанавливается из POSIX-сигнала; читается из scanner retry-цикла. Защита от блокировки
/// SIGTERM на 60-секундном `sleep_for` (D8). После того как `Supervisor::install_signal_handlers`
/// перехватит сигнал, этот флаг продолжает работать как «sticky»-индикатор.
std::atomic<bool> g_early_shutdown{false};

/// Глобальный shared HealthState — обновляется async, читается из HTTP-handlers.
/// Используется для k8s probes (/livez /readyz /healthz). Создаётся в main()
/// и захватывается в lambda HTTP-handler.
std::shared_ptr<tb::app::HealthState> g_health_state;

extern "C" void early_shutdown_handler(int /*sig*/) noexcept {
    // async-signal-safe: только запись в atomic_flag/atomic_bool допустима.
    g_early_shutdown.store(true, std::memory_order_release);
    if (g_health_state) {
        g_health_state->mark_shutdown_requested();
    }
}

/// Sleep, прерываемый ранним shutdown сигналом. Гранулярность опроса: 200 мс.
/// @return true если sleep отработан полностью; false если прерван сигналом.
[[nodiscard]] bool interruptible_sleep(std::chrono::milliseconds total) {
    constexpr auto kPollGranularity = std::chrono::milliseconds(200);
    const auto deadline = std::chrono::steady_clock::now() + total;
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_early_shutdown.load(std::memory_order_acquire)) {
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::min(kPollGranularity, remaining));
    }
    return !g_early_shutdown.load(std::memory_order_acquire);
}

/// Параметры командной строки
struct CliArgs {
    std::string config_path{"configs/production.yaml"};  ///< Путь к файлу конфигурации
    bool        show_help{false};                    ///< Показать справку
    bool        show_version{false};                 ///< Показать версию
};

/// Парсит аргументы командной строки
CliArgs parse_args(std::span<const char*> args) {
    CliArgs result;
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string_view arg{args[i]};
        if (arg == "--help" || arg == "-h") {
            result.show_help = true;
        } else if (arg == "--version" || arg == "-v") {
            result.show_version = true;
        } else if ((arg == "--config" || arg == "-c") && i + 1 < args.size()) {
            result.config_path = args[++i];
        } else if (arg.starts_with("--config=")) {
            result.config_path = std::string{arg.substr(9)};
        }
    }
    return result;
}

/// Выводит справку по использованию
void print_help(std::string_view program_name) {
    std::cout << "Tomorrow Bot — Adaptive Trading System\n\n"
              << "Использование: " << program_name << " [ОПЦИИ]\n\n"
              << "Опции:\n"
              << "  -c, --config=PATH  Путь к файлу конфигурации (по умолчанию: configs/production.yaml)\n"
              << "  -v, --version      Показать версию и выйти\n"
              << "  -h, --help         Показать эту справку\n";
}

/// Выводит версию приложения
void print_version() {
#ifdef TB_VERSION
    std::cout << "Tomorrow Bot v" << TB_VERSION << "\n";
#else
    std::cout << "Tomorrow Bot v0.1.0\n";
#endif
}

} // anonymous namespace

int main(int argc, const char* argv[]) {
    // B7.4 fix: глобальный terminate handler логирует fatal перед crash.
    std::set_terminate([]() {
        std::fprintf(stderr, "[FATAL] std::terminate() called\n");
        try {
            auto eptr = std::current_exception();
            if (eptr) std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[FATAL] uncaught: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[FATAL] uncaught: unknown\n");
        }
        std::abort();
    });

    // ---- 1. Парсинг аргументов ----
    auto args = parse_args(std::span<const char*>{argv, static_cast<std::size_t>(argc)});

    if (args.show_version) {
        print_version();
        return 0;
    }
    if (args.show_help) {
        print_help(argv[0]);
        return 0;
    }

    // ---- 1b. Ранний обработчик SIGTERM/SIGINT ----
    // Устанавливается до scanner-loop, чтобы 60-секундный sleep между retry не блокировал
    // graceful shutdown. После того как Supervisor::install_signal_handlers() переустановит
    // свой handler, флаг остаётся «sticky» и продолжает наблюдаться оставшимся scanner-loop'ом.
    std::signal(SIGTERM, &early_shutdown_handler);
    std::signal(SIGINT,  &early_shutdown_handler);

    // ---- 2. Инициализация компонентов ----
    tb::app::AppBootstrap bootstrap;
    auto components_result = bootstrap.initialize(args.config_path);
    if (!components_result) {
        std::cerr << "[FATAL] Не удалось инициализировать компоненты приложения\n"
                  << "Конфигурация: " << args.config_path << "\n"
                  << "Ошибка: " << tb::TbErrorCategory::instance().message(
                        static_cast<int>(components_result.error())) << "\n";
        return 1;
    }

    auto& comp = *components_result;
    auto& logger  = comp.logger;
    auto& config  = comp.config;

    // ---- 2b. Инициализация HealthState (k8s probes) ----
    g_health_state = std::make_shared<tb::app::HealthState>();
    g_health_state->set_startup_epoch_ns(comp.clock->now().get());

    // ---- 3. Вывод баннера запуска ----
    logger->info("main", "Запуск Tomorrow Bot",
        {{"config", args.config_path},
         {"mode",   std::string(tb::to_string(config.trading.mode))},
         {"config_hash", config.config_hash}});

    // ---- 3b. Immutable runtime manifest ----
    auto manifest = tb::app::RuntimeManifest::build(
        config.config_hash,
        config.exchange.endpoint_rest,
        comp.clock->now().get());
    logger->info("main", "Runtime manifest",
        {{"git_sha", manifest.git_sha},
         {"version", manifest.version},
         {"build_type", manifest.build_type},
         {"config_hash", manifest.config_hash},
         {"exchange", manifest.exchange_endpoint}});

    std::unique_ptr<tb::app::HttpEndpointServer> metrics_server;
    if (config.metrics.enabled) {
        metrics_server = std::make_unique<tb::app::HttpEndpointServer>(
            "127.0.0.1",
            config.metrics.port,
            "metrics_http",
            [metrics = comp.metrics,
             metrics_path = config.metrics.path,
             health = g_health_state,
             clock = comp.clock](std::string_view method,
                                  std::string_view target) {
                const auto query_pos = target.find('?');
                const auto path = target.substr(0, query_pos);
                if (method != "GET") {
                    tb::app::HttpResponse response;
                    response.status_code = 405;
                    response.status_text = "Method Not Allowed";
                    response.body = "Method Not Allowed\n";
                    response.headers.emplace_back("Allow", "GET");
                    return response;
                }

                // ── /livez — liveness probe (k8s) ──
                // Возвращает 200 пока процесс не получил SIGTERM. При SIGTERM → 503,
                // что заставит k8s перезапустить под если приложение не завершилось.
                if (path == "/livez") {
                    tb::app::HttpResponse r;
                    if (health && health->is_alive()) {
                        r.status_code = 200; r.status_text = "OK"; r.body = "alive\n";
                    } else {
                        r.status_code = 503; r.status_text = "Service Unavailable"; r.body = "shutting_down\n";
                    }
                    r.headers.emplace_back("Cache-Control", "no-store");
                    return r;
                }

                // ── /readyz — readiness probe ──
                // Готов принимать торговый трафик: subsystems started, ≥1 pipeline connected,
                // нет kill-switch, не degraded, нет shutdown-флага.
                if (path == "/readyz" || path == "/healthz") {
                    tb::app::HttpResponse r;
                    r.headers.emplace_back("Cache-Control", "no-store");
                    if (health && health->is_ready()) {
                        r.status_code = 200; r.status_text = "OK";
                        r.body = "ready\n"
                                 "connected_pipelines=" +
                                 std::to_string(health->connected_pipeline_count()) +
                                 "\nregistered_pipelines=" +
                                 std::to_string(health->registered_pipeline_count()) +
                                 "\nuptime_ms=" +
                                 std::to_string(clock
                                     ? (clock->now().get() - health->startup_epoch_ns()) / 1'000'000
                                     : 0) +
                                 "\n";
                    } else {
                        r.status_code = 503; r.status_text = "Service Unavailable";
                        std::string reason;
                        if (!health) reason = "no_health_state";
                        else if (health->is_kill_switch_active()) reason = "kill_switch_active";
                        else if (health->is_degraded()) reason = "degraded";
                        else if (!health->is_alive()) reason = "shutdown_requested";
                        else if (health->connected_pipeline_count() == 0) reason = "no_connected_pipelines";
                        else reason = "subsystems_not_started";
                        r.body = "not_ready: " + reason + "\n";
                    }
                    return r;
                }

                if (path != metrics_path) {
                    tb::app::HttpResponse response;
                    response.status_code = 404;
                    response.status_text = "Not Found";
                    response.body = "Not Found\n";
                    return response;
                }

                tb::app::HttpResponse response;
                response.content_type = "text/plain; version=0.0.4; charset=utf-8";
                response.headers.emplace_back("Cache-Control", "no-store");
                response.body = metrics->export_prometheus();
                return response;
            },
            logger);

        if (metrics_server->start()) {
            logger->info("main", "Metrics endpoint запущен",
                {{"address", "127.0.0.1"},
                 {"port", std::to_string(config.metrics.port)},
                 {"path", config.metrics.path}});
        } else {
            logger->error("main", "Не удалось запустить metrics endpoint",
                {{"port", std::to_string(config.metrics.port)},
                 {"path", config.metrics.path}});
            metrics_server.reset();
        }
    }

    // ---- 3.5. EDGE-30-DYNCAP (2026-05-15): Динамический capital sync с биржи ----
    // Получаем актуальный equity для startup pair filtering + allocator config.
    // config.trading.initial_capital — только fallback если exchange fetch fails.
    // После startup Portfolio sync продолжает обновлять capital периодически.
    {
        auto api_key_res = comp.secret_provider->get_secret(tb::security::SecretRef{"BITGET_API_KEY"});
        auto api_secret_res = comp.secret_provider->get_secret(tb::security::SecretRef{"BITGET_API_SECRET"});
        auto api_pass_res = comp.secret_provider->get_secret(tb::security::SecretRef{"BITGET_PASSPHRASE"});

        if (api_key_res && api_secret_res && api_pass_res) {
            auto auth_rest = std::make_shared<tb::exchange::bitget::BitgetRestClient>(
                config.exchange.endpoint_rest, *api_key_res, *api_secret_res, *api_pass_res,
                logger, config.exchange.timeout_ms,
                config.pair_selection.api_retry_max,
                config.pair_selection.api_retry_base_delay_ms);

            std::string balance_query = "productType=" + config.futures.product_type;
            auto resp = auth_rest->get("/api/v2/mix/account/accounts", balance_query);

            double real_balance = -1.0;
            if (resp.success) {
                try {
                    auto doc = boost::json::parse(resp.body);
                    const auto& root = doc.as_object();
                    std::string code(root.at("code").as_string());
                    if (code == "00000") {
                        const auto& data = root.at("data").as_array();
                        double total = 0.0;
                        for (const auto& item : data) {
                            const auto& acc = item.as_object();
                            const auto& eq = acc.at("usdtEquity");
                            total += eq.is_string()
                                ? std::stod(std::string(eq.as_string()))
                                : eq.as_double();
                        }
                        real_balance = total;
                    }
                } catch (const std::exception& ex) {
                    logger->warn("main", "Ошибка парсинга balance ответа",
                        {{"exception", ex.what()}});
                }
            }

            if (real_balance > 0.0) {
                double old_capital = config.trading.initial_capital;
                config.trading.initial_capital = real_balance;
                logger->info("main", "Capital синхронизирован с биржей на startup (EDGE-30-DYNCAP)",
                    {{"exchange_balance_usdt", std::to_string(real_balance)},
                     {"config_value_overridden", std::to_string(old_capital)}});
            } else {
                logger->warn("main", "Exchange balance fetch failed — fallback на config",
                    {{"config_value", std::to_string(config.trading.initial_capital)}});
            }
        } else {
            logger->warn("main", "API keys недоступны — используется config.initial_capital",
                {{"value", std::to_string(config.trading.initial_capital)}});
        }
    }

    // ---- 4. Сканирование торговых пар (ScannerEngine v2) ----
    // Создаём REST-клиент для сканера (публичный, без аутентификации)
    auto scanner_rest_client = std::make_shared<tb::exchange::bitget::BitgetRestClient>(
        config.exchange.endpoint_rest, "", "", "", logger, config.exchange.timeout_ms,
        config.pair_selection.api_retry_max,
        config.pair_selection.api_retry_base_delay_ms);

    // Создаём ScannerConfig из существующей конфигурации
    tb::scanner::ScannerConfig scanner_cfg;
    scanner_cfg.top_n = config.pair_selection.top_n;
    scanner_cfg.blacklist = config.pair_selection.blacklist;
    scanner_cfg.product_type = config.futures.product_type;
    scanner_cfg.rotation_interval_hours = config.pair_selection.rotation_interval_hours;
    // Предфильтр должен быть НАМНОГО мягче глубокого фильтра — пропускаем больше кандидатов
    // для детального анализа (orderbook + candles). 20% от min_volume = разумный компромисс.
    scanner_cfg.prefilter_min_volume_usdt = std::max(config.pair_selection.min_volume_usdt * 0.2, 500'000.0);
    scanner_cfg.prefilter_max_spread_bps = config.pair_selection.max_spread_bps * 3.0;  // pre-filter 3x looser
    scanner_cfg.min_volume_usdt = config.pair_selection.min_volume_usdt;
    scanner_cfg.max_spread_bps = config.pair_selection.max_spread_bps;
    scanner_cfg.max_candidates_detailed = config.pair_selection.max_candidates_for_candles;
    scanner_cfg.api_retry_max = config.pair_selection.api_retry_max;
    scanner_cfg.api_retry_base_delay_ms = config.pair_selection.api_retry_base_delay_ms;
    scanner_cfg.min_orderbook_depth_usdt = config.pair_selection.min_liquidity_depth_usdt;

    // ── Scan timeout & circuit breaker ──
    scanner_cfg.scan_timeout_ms = config.pair_selection.scan_timeout_ms;
    scanner_cfg.circuit_breaker_threshold = config.pair_selection.circuit_breaker_threshold;
    scanner_cfg.circuit_breaker_reset_ms = config.pair_selection.circuit_breaker_reset_ms;

    // ── Basket diversification ──
    scanner_cfg.enable_diversification = config.pair_selection.enable_diversification;
    scanner_cfg.max_correlation_in_basket = config.pair_selection.max_correlation_in_basket;
    scanner_cfg.max_pairs_per_sector = config.pair_selection.max_pairs_per_sector;

    // ИСПРАВЛЕНИЕ H10: пробросить scorer-настройки из YAML в ScannerConfig
    scanner_cfg.filter_min_change_24h = config.pair_selection.scorer.filter_min_change_24h;
    scanner_cfg.filter_max_change_24h = config.pair_selection.scorer.filter_max_change_24h;

    // ИСПРАВЛЕНИЕ H3 (аудит): пробросить дополнительные scorer-поля в ScannerConfig,
    // чтобы YAML-параметры реально влияли на runtime фильтрацию и ранжирование.
    const auto& sc = config.pair_selection.scorer;
    scanner_cfg.min_volume_usdt = std::max(scanner_cfg.min_volume_usdt, sc.volume_tier_minimal);
    scanner_cfg.min_volatility_pct = std::max(sc.volatility_low_threshold,
        tb::common::fees::kEconomicAtrFloorPct);
    scanner_cfg.max_volatility_pct = sc.volatility_high_threshold;

    // D12 fix: микро-аккаунт scaling вынесен в ScannerConfig (порог + ослабленные значения).
    // Стратегия защищает $5-$10 ордера от строгих сетевых фильтров рассчитанных на $1K+ объёмы.
    // Также устраняется deadzone: trade_state_max_trap_risk должен совпадать с max_trap_risk,
    // иначе пары проходят фильтр (trap < 0.7) но получают DoNotTrade (trap > 0.6).
    if (config.trading.initial_capital < scanner_cfg.micro_account_capital_threshold_usdt) {
        scanner_cfg.min_orderbook_depth_usdt = scanner_cfg.micro_min_orderbook_depth_usdt;
        scanner_cfg.min_open_interest_usdt   = scanner_cfg.micro_min_open_interest_usdt;
        scanner_cfg.trade_state_max_trap_risk = scanner_cfg.max_trap_risk;
        logger->info("main", "Применён micro-account scaling сканера",
            {{"initial_capital_usdt", std::to_string(config.trading.initial_capital)},
             {"threshold_usdt", std::to_string(scanner_cfg.micro_account_capital_threshold_usdt)},
             {"min_depth_usdt", std::to_string(scanner_cfg.min_orderbook_depth_usdt)},
             {"min_oi_usdt", std::to_string(scanner_cfg.min_open_interest_usdt)}});
    }

    // Создаём ScannerEngine (v2: features, traps, bias, ranking)
    auto market_scanner = std::make_shared<tb::scanner::ScannerEngine>(
        scanner_cfg, scanner_rest_client, logger, comp.metrics);

    // Выполняем сканирование с ретраями — если рынок не даёт пар, ждём
    tb::scanner::ScannerResult scanner_result;
    std::vector<std::string> active_symbols;
    // BUG-S23-05: track symbols force-added from open positions so capital filter
    // excludes them from budget calculation (they inflate n, deflating per-pipeline budget).
    // These symbols are re-appended after filtering since open positions must always be managed.
    std::unordered_set<std::string> forced_by_position_symbols;

    if (config.pair_selection.mode == tb::config::PairSelectionMode::Manual &&
        !config.pair_selection.manual_symbols.empty()) {
        // Ручной режим: используем заданные символы, пропускаем сканер
        active_symbols = config.pair_selection.manual_symbols;
        std::string syms_str;
        for (const auto& s : active_symbols) {
            if (!syms_str.empty()) syms_str += ", ";
            syms_str += s;
        }
        logger->info("main", "Ручной режим: используем заданные символы",
            {{"symbols", syms_str},
             {"count", std::to_string(active_symbols.size())}});
    } else {
        logger->info("main", "Запуск сканирования торговых пар (ScannerEngine v2)...");

        constexpr int kScanRetryIntervalSec = 60;
        constexpr int kMaxScanRetries = 30;  // макс 30 мин ожидания

        for (int attempt = 1; attempt <= kMaxScanRetries; ++attempt) {
            scanner_result = market_scanner->scan();
            active_symbols = scanner_result.selected_tradable_symbols();

            if (!active_symbols.empty()) {
                for (const auto& p : scanner_result.top_pairs) {
                    logger->info("main", "Scanner v2: " + p.symbol +
                        " | Score=" + std::to_string(p.score.total) +
                        " | Bias=" + std::string(tb::scanner::to_string(p.bias)) +
                        " | State=" + std::string(tb::scanner::to_string(p.trade_state)));
                }
                break;
            }

            if (attempt == kMaxScanRetries) {
                logger->critical("main", "Сканер не нашёл подходящих пар после " +
                    std::to_string(kMaxScanRetries) + " попыток. Завершение.");
                return 1;
            }

            logger->warn("main", "Сканер не нашёл подходящих пар, повторная попытка через " +
                std::to_string(kScanRetryIntervalSec) + " сек",
                {{"attempt", std::to_string(attempt)},
                 {"max_attempts", std::to_string(kMaxScanRetries)},
                 {"errors", std::to_string(scanner_result.errors.size())}});

            // D8 fix: shutdown-aware sleep. Возврат false означает SIGTERM/SIGINT во время сна.
            if (!interruptible_sleep(std::chrono::seconds(kScanRetryIntervalSec))) {
                logger->info("main", "SIGTERM/SIGINT во время scanner-retry — корректный выход");
                return 0;
            }
        }
    }

    // ---- 4.5. Проверка уже открытых позиций на бирже ----
    {
        try {
            auto api_key_res = comp.secret_provider->get_secret(tb::security::SecretRef{"BITGET_API_KEY"});
            auto api_secret_res = comp.secret_provider->get_secret(tb::security::SecretRef{"BITGET_API_SECRET"});
            auto passphrase_res = comp.secret_provider->get_secret(tb::security::SecretRef{"BITGET_PASSPHRASE"});

            if (api_key_res && api_secret_res && passphrase_res) {
                auto auth_rest = std::make_shared<tb::exchange::bitget::BitgetRestClient>(
                    config.exchange.endpoint_rest,
                    *api_key_res, *api_secret_res, *passphrase_res,
                    logger, config.exchange.timeout_ms,
                    config.pair_selection.api_retry_max,
                    config.pair_selection.api_retry_base_delay_ms);

                {
                    tb::exchange::bitget::BitgetFuturesQueryAdapter futures_query{
                        auth_rest, logger, config.futures};
                    auto open_positions = futures_query.get_open_positions();
                    std::unordered_set<std::string> protected_symbols;
                    if (!open_positions.has_value()) {
                        logger->warn("main", "Не удалось загрузить открытые фьючерсные позиции",
                            {{"error", tb::TbErrorCategory::instance().message(
                                static_cast<int>(open_positions.error()))}});
                    } else {
                        std::string positions_str;
                        for (const auto& position : open_positions.value()) {
                            const std::string symbol = position.symbol.get();
                            if (symbol.empty() || position.size.get() <= 0.0) {
                                continue;
                            }
                            protected_symbols.insert(symbol);

                            if (!positions_str.empty()) {
                                positions_str += ", ";
                            }
                            positions_str += symbol + " " + std::string(tb::to_string(position.side));

                            // Если есть открытая позиция по символу не из сканера — добавляем
                            bool already_tracked = false;
                            for (const auto& s : active_symbols) {
                                if (s == symbol) { already_tracked = true; break; }
                            }
                            if (!already_tracked) {
                                active_symbols.push_back(symbol);
                                forced_by_position_symbols.insert(symbol);  // BUG-S23-05
                                logger->warn("main",
                                    "Принудительно добавлен символ с открытой фьючерсной позицией: " + symbol,
                                    {{"side", std::string(tb::to_string(position.side))},
                                     {"size", std::to_string(position.size.get())},
                                     {"entry_price", std::to_string(position.entry_price.get())},
                                     {"mark_price", std::to_string(position.current_price.get())},
                                     {"notional_usd", std::to_string(position.notional_usd)},
                                     {"unrealized_pnl", std::to_string(position.unrealized_pnl)}});
                            }
                        }

                        if (!positions_str.empty()) {
                            logger->info("main", "Открытые фьючерсные позиции на бирже: " + positions_str);
                        }
                    }

                    // ---- Глобальный startup wipe (SMART v3 — CONSERVATIVE) ----
                    // Стратегия:
                    //   • protected_pos = (symbol, side) с открытой позицией.
                    //   • Plan-orders для protected_pos → ВООБЩЕ НЕ ТРОГАЕМ.
                    //     Они могут быть нужны для защиты позиции (preset TPSL,
                    //     standalone fallback, trailing replace). Если есть лишние —
                    //     periodic cleanup_orphans разберётся (с tracked_ids защитой).
                    //   • Plan-orders НЕ для protected → ВСЕ отменяем (orphans).
                    //   • Regular pending orders → ВСЕ отменяем (orphans entry).
                    //
                    // Это безопасный подход: лучше иметь несколько duplicate plan-orders
                    // для активных позиций, чем оставить позицию без защиты.
                    try {
                        auto submitter = std::make_shared<
                            tb::exchange::bitget::BitgetFuturesOrderSubmitter>(
                            auth_rest, logger, config.futures);

                        struct PositionKey {
                            std::string symbol;
                            tb::PositionSide side;
                            bool operator==(const PositionKey& o) const {
                                return symbol == o.symbol && side == o.side;
                            }
                        };
                        struct PositionKeyHash {
                            size_t operator()(const PositionKey& k) const {
                                return std::hash<std::string>{}(k.symbol)
                                     ^ (static_cast<size_t>(k.side) << 1);
                            }
                        };
                        std::unordered_set<PositionKey, PositionKeyHash> protected_pos;
                        std::unordered_set<std::string> protected_symbols_set;
                        if (open_positions.has_value()) {
                            for (const auto& pos : open_positions.value()) {
                                if (pos.symbol.get().empty() || pos.size.get() <= 0.0) continue;
                                const tb::PositionSide ps = (pos.side == tb::Side::Buy)
                                    ? tb::PositionSide::Long : tb::PositionSide::Short;
                                protected_pos.insert({pos.symbol.get(), ps});
                                protected_symbols_set.insert(pos.symbol.get());
                            }
                        }

                        int wiped_plans = 0, wiped_regs = 0, kept_plans = 0;

                        auto plans = futures_query.get_open_plan_orders(tb::Symbol(""));
                        if (plans) {
                            for (const auto& p : *plans) {
                                PositionKey pk{p.symbol.get(), p.position_side};
                                if (protected_pos.count(pk) > 0) {
                                    // НЕ ТРОГАЕМ — может быть нашей защитой.
                                    ++kept_plans;
                                    continue;
                                }
                                const std::string& pt = p.plan_type.empty()
                                    ? std::string("normal_plan") : p.plan_type;
                                if (submitter->cancel_plan_order(p.order_id, p.symbol, pt)) {
                                    ++wiped_plans;
                                }
                            }
                        }

                        auto regs = futures_query.get_open_orders(tb::Symbol(""));
                        if (regs) {
                            for (const auto& o : *regs) {
                                // Regular pending — даже для protected positions это
                                // entry orders которые могли застрять. Отменяем все.
                                if (submitter->cancel_order(o.order_id, o.symbol)) {
                                    ++wiped_regs;
                                }
                            }
                        }

                        if (wiped_plans > 0 || wiped_regs > 0 || kept_plans > 0) {
                            std::string prot_str;
                            for (const auto& pk : protected_pos) {
                                if (!prot_str.empty()) prot_str += ",";
                                prot_str += pk.symbol + ":" +
                                    (pk.side == tb::PositionSide::Long ? "Long" : "Short");
                            }
                            logger->warn("main",
                                "Global startup wipe (smart v3: conservative)",
                                {{"plan_cancelled", std::to_string(wiped_plans)},
                                 {"plan_kept_for_positions", std::to_string(kept_plans)},
                                 {"regular_cancelled", std::to_string(wiped_regs)},
                                 {"protected_positions", prot_str}});
                        } else {
                            logger->info("main",
                                "Global startup wipe: висящих ордеров не найдено");
                        }
                    } catch (const std::exception& e) {
                        logger->warn("main", "Global startup wipe не выполнен",
                            {{"error", e.what()}});
                    }
                }
            }
        } catch (const std::exception& e) {
            logger->warn("main", "Не удалось проверить уже открытые позиции на бирже",
                {{"error", e.what()}});
        }
    }

    {
        std::string symbols_str;
        for (const auto& s : active_symbols) {
            if (!symbols_str.empty()) symbols_str += ", ";
            symbols_str += s;
        }
        logger->info("main", "Финальный список символов: " + std::to_string(active_symbols.size()),
            {{"symbols", symbols_str}});
    }

    // Собираем правила инструментов из результатов сканирования
    std::unordered_map<std::string, tb::ExchangeSymbolRules> symbol_rules;

    // Из нового ScannerEngine (top_pairs + rejected содержат precision)
    for (const auto& sa : scanner_result.top_pairs) {
        tb::ExchangeSymbolRules rules;
        rules.symbol = tb::Symbol(sa.symbol);
        rules.quantity_precision = sa.quantity_precision;
        rules.price_precision = sa.price_precision;
        rules.min_trade_usdt = sa.min_trade_usdt;
        rules.min_quantity = sa.min_quantity;
        symbol_rules[sa.symbol] = rules;
    }
    for (const auto& sa : scanner_result.rejected_pairs) {
        if (symbol_rules.count(sa.symbol)) continue;
        tb::ExchangeSymbolRules rules;
        rules.symbol = tb::Symbol(sa.symbol);
        rules.quantity_precision = sa.quantity_precision;
        rules.price_precision = sa.price_precision;
        rules.min_trade_usdt = sa.min_trade_usdt;
        rules.min_quantity = sa.min_quantity;
        symbol_rules[sa.symbol] = rules;
    }

    // ---- 4.5. Проверка доступности пар по капиталу ----
    // Используем per-pipeline бюджет (капитал / N_пар * плечо * 0.8),
    // отфильтровываем пары, для которых минимальный ордер превышает бюджет.
    // Итеративно пересчитываем, т.к. удаление пары увеличивает бюджет остальных.
    {
        const double total_capital = config.trading.initial_capital;
        const int max_leverage = config.futures.max_leverage;

        // BUG-S23-05: temporarily remove forced-by-position symbols from active_symbols
        // so they don't inflate n and shrink per_pipeline_budget for normal symbols.
        std::vector<std::string> position_forced_temp;
        if (!forced_by_position_symbols.empty()) {
            std::vector<std::string> normal_symbols;
            for (const auto& sym : active_symbols) {
                if (forced_by_position_symbols.count(sym)) {
                    position_forced_temp.push_back(sym);
                } else {
                    normal_symbols.push_back(sym);
                }
            }
            active_symbols = std::move(normal_symbols);
        }

        bool changed = true;
        while (changed && !active_symbols.empty()) {
            changed = false;
            int n = static_cast<int>(active_symbols.size());
            double per_pipeline_budget = (total_capital / n) * max_leverage * 0.8;

            for (auto it = active_symbols.begin(); it != active_symbols.end(); ) {
                const auto& sym = *it;
                auto rules_it = symbol_rules.find(sym);
                if (rules_it == symbol_rules.end()) { ++it; continue; }
                const auto& rules = rules_it->second;
                double step = std::pow(10.0, -rules.quantity_precision);
                double min_qty = std::max(rules.min_quantity, step);
                double price = 0.0;
                for (const auto& sa : scanner_result.top_pairs) {
                    if (sa.symbol == sym) { price = sa.last_price; break; }
                }
                if (price <= 0.0) { ++it; continue; }
                double min_notional = min_qty * price;
                if (min_notional > per_pipeline_budget) {
                    logger->warn("main", "Пара отфильтрована: минимальный ордер превышает бюджет",
                        {{"symbol", sym},
                         {"min_qty", std::to_string(min_qty)},
                         {"price", std::to_string(price)},
                         {"min_notional", std::to_string(min_notional)},
                         {"per_pipeline_budget", std::to_string(per_pipeline_budget)}});
                    it = active_symbols.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
        if (active_symbols.empty()) {
            logger->error("main", "Нет доступных по капиталу пар! Завершение.");
            return 1;
        }

        // BUG-S23-05: re-add forced position symbols after capital filtering
        for (const auto& sym : position_forced_temp) {
            active_symbols.push_back(sym);
        }
    }

    // ---- 5. Создание и запуск супервизора ----
    tb::supervisor::Supervisor supervisor{
        comp.logger,
        comp.metrics,
        comp.clock
    };

    supervisor.install_signal_handlers();

    // ---- 5.5. Единый account-level портфель (shared между всеми pipeline) ----
    auto shared_portfolio = std::make_shared<tb::portfolio::InMemoryPortfolioEngine>(
        config.trading.initial_capital, comp.logger, comp.clock, comp.metrics);

    // ---- 5.6. Создание торговых pipeline для КАЖДОЙ выбранной пары ----
    std::vector<std::shared_ptr<tb::pipeline::TradingPipeline>> pipelines;
    pipelines.reserve(active_symbols.size());

    for (size_t i = 0; i < active_symbols.size(); ++i) {
        const auto& sym = active_symbols[i];

        auto pipeline = std::make_shared<tb::pipeline::TradingPipeline>(
            config, comp.secret_provider, comp.logger, comp.clock, comp.metrics,
            sym, shared_portfolio);

        // Устанавливаем правила инструмента из данных сканирования
        auto rules_it = symbol_rules.find(sym);
        if (rules_it != symbol_rules.end()) {
            pipeline->set_exchange_rules(rules_it->second);
        }

        // Устанавливаем количество pipeline для корректного деления капитала
        pipeline->set_num_pipelines(static_cast<int>(active_symbols.size()));

        // D3 fix: подключаем authoritative kill-switch provider от Supervisor.
        // RiskEngine на каждом evaluate синхронизирует local state с этим источником,
        // закрывая race window между global activation и доставкой в listener'ы.
        pipeline->set_kill_switch_provider([sup = &supervisor]() {
            return std::pair<bool, std::string>{
                sup->is_kill_switch_active(),
                sup->kill_switch_reason()
            };
        });

        // Регистрируем pipeline как подсистему supervisor с уникальным именем
        std::string subsystem_name = "pipeline_" + sym;
        supervisor.register_subsystem(subsystem_name,
            [pipeline]() { return pipeline->start(); },
            [pipeline]() { pipeline->stop(); });

        pipelines.push_back(std::move(pipeline));

        logger->info("main", "Pipeline создан для " + sym,
            {{"index", std::to_string(i + 1)},
             {"total", std::to_string(active_symbols.size())}});
    }

    // ---- 5.7. Запуск ротации пар (каждые N часов) ----
    // Ротация через новый ScannerEngine (только в auto режиме)
    if (config.pair_selection.mode != tb::config::PairSelectionMode::Manual) {
        market_scanner->start_rotation([&logger](const tb::scanner::ScannerResult& result) {
            auto syms = result.selected_tradable_symbols();
            std::string symbols_str;
            for (const auto& s : syms) {
                if (!symbols_str.empty()) symbols_str += ", ";
                symbols_str += s;
            }
            logger->info("main", "Плановая ротация (ScannerEngine v2). Лучшие пары: " + symbols_str,
                {{"count", std::to_string(syms.size())},
                 {"scan_duration_ms", std::to_string(result.scan_duration_ms)}});
        });
    }

    try {
        supervisor.start();
    } catch (const std::exception& ex) {
        logger->critical("main", "Критическая ошибка при запуске",
            {{"error", ex.what()}});
        return 2;
    }

    // ---- 5.7b. Health probes — отображаем готовность системы ----
    g_health_state->mark_subsystems_started();
    g_health_state->set_registered_pipeline_count(static_cast<int>(pipelines.size()));

    // Background thread обновляет HealthState (connected_pipelines, kill_switch, degraded).
    // Lock-free atomic писатель; HTTP-handlers читают тем же atomic'ами.
    std::atomic<bool> health_monitor_running{true};
    std::jthread health_monitor_thread{[&supervisor, &pipelines, &health_monitor_running](std::stop_token st) {
        while (!st.stop_requested() && health_monitor_running.load(std::memory_order_acquire)) {
            int connected = 0;
            for (const auto& p : pipelines) {
                if (p && p->is_connected()) ++connected;
            }
            if (g_health_state) {
                g_health_state->set_connected_pipeline_count(connected);
                g_health_state->set_kill_switch_active(supervisor.is_kill_switch_active());
                g_health_state->set_degraded(supervisor.is_degraded());
                g_health_state->set_registered_pipeline_count(
                    static_cast<int>(pipelines.size()));
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }};

    // ---- 5.8. Динамическая ротация при простое pipeline ----
    // В ручном режиме (Manual) ротация отключена — всегда торгуем заданными символами.
    // В автоматическом режиме: фоновый поток проверяет каждые 5 минут: если ВСЕ pipeline простаивают
    // (нет торговли > 30 мин) и нет открытых позиций — пересканировать рынок
    // и заменить pipeline на новые символы.
    const bool rotation_enabled = (config.pair_selection.mode != tb::config::PairSelectionMode::Manual);
    // run90: thresholds теперь из config. Дефолты: idle 5 min, check 30s, rescan 5 min.
    const int64_t kIdleThresholdNs =
        static_cast<int64_t>(config.pair_selection.idle_threshold_minutes) * 60LL * 1'000'000'000LL;
    const int kIdleCheckIntervalSec = config.pair_selection.idle_check_interval_sec;
    const int64_t kMinRescanIntervalNs =
        static_cast<int64_t>(config.pair_selection.min_rescan_interval_minutes) * 60LL * 1'000'000'000LL;

    std::atomic<bool> idle_monitor_running{true};
    int64_t last_rescan_ns = comp.clock->now().get();

    // Log active symbols before starting idle_monitor_thread so the read of
    // active_symbols happens only on the main thread (BUG-S12-01/02/03).
    {
        std::string all_symbols;
        for (const auto& s : active_symbols) {
            if (!all_symbols.empty()) all_symbols += ", ";
            all_symbols += s;
        }
        logger->info("main", "Система запущена. Ожидание сигнала завершения (SIGTERM/SIGINT)...",
            {{"active_pairs", all_symbols},
             {"pair_count", std::to_string(active_symbols.size())}});
    }

    std::thread idle_monitor_thread([&]() {
        while (idle_monitor_running.load()) {
            // Спим по 10 секунд, проверяя флаг остановки
            for (int elapsed = 0; elapsed < kIdleCheckIntervalSec && idle_monitor_running.load();
                 elapsed += 10) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
            if (!idle_monitor_running.load()) break;

            // В ручном режиме ротация отключена
            if (!rotation_enabled) continue;

            // Throttle: не ресканировать чаще чем раз в 30 мин
            int64_t now_ns = comp.clock->now().get();
            if ((now_ns - last_rescan_ns) < kMinRescanIntervalNs) continue;

            // Проверяем: есть ли pipeline без позиций, которые простаивают > порога
            bool any_idle_without_position = false;
            bool any_has_position = false;
            int idle_count = 0;
            for (const auto& p : pipelines) {
                if (p->has_open_position()) {
                    any_has_position = true;
                } else if (p->is_idle(kIdleThresholdNs)) {
                    any_idle_without_position = true;
                    ++idle_count;
                }
            }

            // Ресканируем если хотя бы один pipeline простаивает без позиции
            if (!any_idle_without_position) continue;

            logger->warn("main", "Простаивающие pipeline обнаружены. Запуск ресканирования...",
                {{"pipeline_count", std::to_string(pipelines.size())},
                 {"idle_count", std::to_string(idle_count)},
                 {"has_positions", any_has_position ? "true" : "false"}});

            try {
                // Используем новый ScannerEngine для ресканирования
                auto rescan_v2 = market_scanner->scan();

                // PROFESSIONAL ROTATION POOL (user 2026-05-18):
                // На тихом рынке top_pairs может быть пуст или совпадать с active.
                // Собираем полный pool кандидатов:
                //   1. top_pairs (TradeAllowed) — лучшие по score
                //   2. rejected_pairs (Neutral) — следующие по качеству
                //   3. rejected_pairs (DoNotTrade) — последний резерв (только если рынок мёртв)
                // Сортируем по score descending — даём idle slots самых перспективных
                // кандидатов даже если они не прошли strict-фильтры.
                struct PoolEntry {
                    std::string symbol;
                    double score;
                    int tier;  // 0=top, 1=rejected-neutral, 2=rejected-donottrade
                };
                std::vector<PoolEntry> rotation_pool;
                rotation_pool.reserve(rescan_v2.top_pairs.size() + rescan_v2.rejected_pairs.size());
                for (const auto& sa : rescan_v2.top_pairs) {
                    rotation_pool.push_back({sa.symbol, sa.score.total, 0});
                }
                for (const auto& sa : rescan_v2.rejected_pairs) {
                    const int tier = (sa.trade_state == tb::scanner::TradeState::Neutral) ? 1 : 2;
                    rotation_pool.push_back({sa.symbol, sa.score.total, tier});
                }
                // Сортировка: сначала по tier (top > neutral > donottrade), потом по score.
                std::sort(rotation_pool.begin(), rotation_pool.end(),
                    [](const PoolEntry& a, const PoolEntry& b) {
                        if (a.tier != b.tier) return a.tier < b.tier;
                        return a.score > b.score;
                    });

                std::vector<std::string> rescan_syms;
                rescan_syms.reserve(rotation_pool.size());
                for (const auto& e : rotation_pool) rescan_syms.push_back(e.symbol);

                logger->info("main", "Rescan pool собран",
                    {{"top", std::to_string(rescan_v2.top_pairs.size())},
                     {"rejected", std::to_string(rescan_v2.rejected_pairs.size())},
                     {"total_pool", std::to_string(rescan_syms.size())}});

                if (rescan_syms.empty()) {
                    logger->warn("main", "Ресканирование не нашло НИ ОДНОЙ пары (биржа недоступна?)");
                    last_rescan_ns = comp.clock->now().get();
                    continue;
                }

                // Частичная ротация: заменяем только простаивающие pipeline без позиций
                // Pipeline с открытыми позициями продолжают работать.

                // 1. Обновляем правила инструментов из нового сканирования
                for (const auto& sa : rescan_v2.top_pairs) {
                    tb::ExchangeSymbolRules rules;
                    rules.symbol = tb::Symbol(sa.symbol);
                    rules.quantity_precision = sa.quantity_precision;
                    rules.price_precision = sa.price_precision;
                    rules.min_trade_usdt = sa.min_trade_usdt;
                    rules.min_quantity = sa.min_quantity;
                    symbol_rules[sa.symbol] = rules;
                }
                for (const auto& sa : rescan_v2.rejected_pairs) {
                    if (symbol_rules.count(sa.symbol)) continue;
                    tb::ExchangeSymbolRules rules;
                    rules.symbol = tb::Symbol(sa.symbol);
                    rules.quantity_precision = sa.quantity_precision;
                    rules.price_precision = sa.price_precision;
                    rules.min_trade_usdt = sa.min_trade_usdt;
                    rules.min_quantity = sa.min_quantity;
                    symbol_rules[sa.symbol] = rules;
                }

                // 2. Определяем какие символы уже активны (и не будут заменены)
                // EDGE-30-SOFT (user request 2026-05-15): MILD replacement — pipelines
                // с open position НИКОГДА не заменяются при rotation. Ждём natural exit.
                std::vector<size_t> idle_indices;
                std::unordered_set<std::string> kept_symbols;
                int held_open_count = 0;
                int held_active_count = 0;
                for (size_t i = 0; i < pipelines.size(); ++i) {
                    bool has_pos = pipelines[i]->has_open_position();
                    bool is_idle = pipelines[i]->is_idle(kIdleThresholdNs);
                    if (!has_pos && is_idle) {
                        idle_indices.push_back(i);
                    } else {
                        kept_symbols.insert(active_symbols[i]);
                        if (has_pos) ++held_open_count;
                        else ++held_active_count;
                    }
                }
                if (held_open_count > 0) {
                    logger->info("main", "Soft rotation: pipelines с открытыми позициями сохранены — ждём natural exit",
                        {{"held_with_position", std::to_string(held_open_count)},
                         {"held_active_no_position", std::to_string(held_active_count)},
                         {"idle_for_replace", std::to_string(idle_indices.size())}});
                }

                if (idle_indices.empty()) {
                    last_rescan_ns = comp.clock->now().get();
                    continue;
                }

                // 3. Собираем ВСЕ активные символы (включая idle) для проверки дубликатов
                std::unordered_set<std::string> all_active_symbols;
                for (const auto& s : active_symbols) {
                    all_active_symbols.insert(s);
                }

                // 4. Выбираем новые символы из ресканирования (не пересекаются с ЛЮБЫМ активным)
                std::vector<std::string> new_candidates;
                for (const auto& s : rescan_syms) {
                    if (all_active_symbols.find(s) == all_active_symbols.end()) {
                        new_candidates.push_back(s);
                    }
                }

                // 5. Заменяем idle pipeline новыми символами
                size_t replaced = 0;
                for (size_t idx = 0; idx < idle_indices.size() && replaced < new_candidates.size(); ++idx) {
                    size_t pi = idle_indices[idx];
                    const auto& old_sym = active_symbols[pi];
                    const auto& new_sym = new_candidates[replaced];

                    // Idle pipeline \u0437\u0430\u043c\u0435\u043d\u044f\u0435\u0442\u0441\u044f \u0432\u0441\u0435\u0433\u0434\u0430 \u2014 \u0434\u0430\u0436\u0435 \u0435\u0441\u043b\u0438 \u0441\u0438\u043c\u0432\u043e\u043b \u0432 \u0442\u043e\u043f\u0435 \u0441\u043a\u0430\u043d\u0430.\n                    // \u0415\u0441\u043b\u0438 pipeline \u043f\u0440\u043e\u0441\u0442\u0430\u0438\u0432\u0430\u0435\u0442, \u0437\u043d\u0430\u0447\u0438\u0442 \u0441\u0438\u043c\u0432\u043e\u043b \u043d\u0435 \u0434\u0430\u0451\u0442 \u0441\u0435\u0442\u0430\u043f\u043e\u0432 \u2014 \u043f\u0435\u0440\u0435\u0445\u043e\u0434\u0438\u043c \u043d\u0430 \u0434\u0440\u0443\u0433\u043e\u0439.

                    // Двойная проверка: new_sym не должен совпадать с другим активным pipeline
                    if (all_active_symbols.count(new_sym)) {
                        logger->warn("main", "Пропуск ротации: символ уже активен в другом pipeline",
                            {{"symbol", new_sym}});
                        // BUG-NEW-16: do NOT increment `replaced` here — rotation was skipped,
                        // so the slot is still idle and the counter must not advance.
                        continue;
                    }

                    // EDGE-30-SOFT (run79): defensive double-check — НИКОГДА не заменяем
                    // pipeline с открытой позицией, даже если попал в idle_indices race.
                    if (pipelines[pi]->has_open_position()) {
                        logger->warn("main", "Пропуск замены: pipeline имеет позицию (защита soft rotation)",
                            {{"symbol", old_sym}});
                        continue;
                    }
                    logger->warn("main", "Замена pipeline: " + old_sym + " → " + new_sym);

                    // Останавливаем старый pipeline
                    supervisor.unregister_subsystem("pipeline_" + old_sym);
                    pipelines[pi]->stop();

                    // Создаём новый pipeline
                    auto pipeline = std::make_shared<tb::pipeline::TradingPipeline>(
                        config, comp.secret_provider, comp.logger, comp.clock,
                        comp.metrics, new_sym, shared_portfolio);

                    auto rules_it = symbol_rules.find(new_sym);
                    if (rules_it != symbol_rules.end()) {
                        pipeline->set_exchange_rules(rules_it->second);
                    }
                    pipeline->set_num_pipelines(static_cast<int>(active_symbols.size()));
                    // D3: переподключаем authoritative kill-switch при ротации
                    pipeline->set_kill_switch_provider([sup = &supervisor]() {
                        return std::pair<bool, std::string>{
                            sup->is_kill_switch_active(),
                            sup->kill_switch_reason()
                        };
                    });
                    pipeline->start();

                    std::string subsystem_name = "pipeline_" + new_sym;
                    supervisor.register_subsystem(subsystem_name,
                        [pipeline]() { return pipeline->start(); },
                        [pipeline]() { pipeline->stop(); });

                    pipelines[pi] = pipeline;
                    active_symbols[pi] = new_sym;
                    all_active_symbols.erase(old_sym);
                    all_active_symbols.insert(new_sym);
                    ++replaced;
                }

                last_rescan_ns = comp.clock->now().get();
                if (replaced > 0) {
                    std::string all_syms;
                    for (const auto& s : active_symbols) {
                        if (!all_syms.empty()) all_syms += ", ";
                        all_syms += s;
                    }
                    logger->info("main", "Частичная ротация завершена",
                        {{"replaced", std::to_string(replaced)},
                         {"active", all_syms}});
                }

            } catch (const std::exception& e) {
                logger->error("main", "Ошибка при ресканировании: " + std::string(e.what()));
                last_rescan_ns = comp.clock->now().get(); // throttle даже при ошибке
            }
        }
    });

    // ---- 6. Ожидание завершения ----
    supervisor.wait_for_shutdown();
    if (g_health_state) g_health_state->mark_shutdown_requested();
    health_monitor_running.store(false, std::memory_order_release);
    health_monitor_thread.request_stop();

    // ---- 7. Корректное завершение ----
    logger->info("main", "Начало процедуры завершения работы...");
    logger->info("main", "[shutdown] step 1: idle_monitor_thread.join()");
    idle_monitor_running.store(false);
    if (idle_monitor_thread.joinable()) {
        idle_monitor_thread.join();
    }
    logger->info("main", "[shutdown] step 2: market_scanner->stop_rotation()");
    market_scanner->stop_rotation();
    logger->info("main", "[shutdown] step 3: metrics_server->stop()");
    if (metrics_server) {
        metrics_server->stop();
    }
    // Останавливаем pipeline вручную (supervisor может не знать о горячих заменах)
    for (size_t i = 0; i < pipelines.size(); ++i) {
        logger->info("main", "[shutdown] step 4." + std::to_string(i) + ": pipeline->stop() " + pipelines[i]->symbol().get());
        pipelines[i]->stop();
    }
    logger->info("main", "[shutdown] step 5: supervisor.stop()");
    supervisor.stop();
    logger->info("main", "[shutdown] step 6: done");
    if (g_health_state) g_health_state->mark_subsystems_stopped();
    logger->info("main", "Tomorrow Bot завершил работу");

    return 0;
}
