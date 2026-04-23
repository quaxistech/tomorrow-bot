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
#include "http_server.hpp"
#include "runtime_manifest.hpp"
#include "supervisor/supervisor.hpp"
#include "pipeline/trading_pipeline.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "scanner/scanner_engine.hpp"
#include "common/constants.hpp"
#include "common/exchange_rules.hpp"
#include "exchange/bitget/bitget_futures_query_adapter.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "security/secret_provider.hpp"
#include "common/enums.hpp"
#include <boost/json.hpp>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <chrono>

namespace {

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
            [metrics = comp.metrics, metrics_path = config.metrics.path](std::string_view method,
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

    // ---- 4. Сканирование торговых пар (ScannerEngine v2) ----
    // Создаём REST-клиент для сканера (публичный, без аутентификации)
    auto scanner_rest_client = std::make_shared<tb::exchange::bitget::BitgetRestClient>(
        config.exchange.endpoint_rest, "", "", "", logger, config.exchange.timeout_ms);

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
    constexpr double kScannerRoundTripFeePct = tb::common::fees::kDefaultTakerFeePct * 2.0 * 100.0;
    constexpr double kScannerEconomicMinVolPct = kScannerRoundTripFeePct * 1.25;
    scanner_cfg.min_volatility_pct = std::max(sc.volatility_low_threshold, kScannerEconomicMinVolPct);
    scanner_cfg.max_volatility_pct = sc.volatility_high_threshold;

    // Микро-аккаунт: ослабляем фильтры сканера (нам не нужна глубина $50K для $5 ордеров)
    if (config.trading.initial_capital < 100.0) {
        scanner_cfg.min_orderbook_depth_usdt = 5'000.0;   // $5K вместо $50K (наши ордера ~$5)
        scanner_cfg.min_open_interest_usdt = 100'000.0;    // $100K вместо $500K
        // Устраняем deadzone: trade_state_max_trap_risk должен совпадать с max_trap_risk
        // Иначе пары проходят фильтр (trap < 0.7) но получают DoNotTrade (trap > 0.6)
        scanner_cfg.trade_state_max_trap_risk = scanner_cfg.max_trap_risk;  // 0.7
    }

    // Создаём ScannerEngine (v2: features, traps, bias, ranking)
    auto market_scanner = std::make_shared<tb::scanner::ScannerEngine>(
        scanner_cfg, scanner_rest_client, logger, comp.metrics);

    // Выполняем сканирование с ретраями — если рынок не даёт пар, ждём
    tb::scanner::ScannerResult scanner_result;
    std::vector<std::string> active_symbols;

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
            active_symbols = scanner_result.selected_symbols();

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

            std::this_thread::sleep_for(std::chrono::seconds(kScanRetryIntervalSec));
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
                    logger, config.exchange.timeout_ms);

                {
                    tb::exchange::bitget::BitgetFuturesQueryAdapter futures_query{
                        auth_rest, logger, config.futures};
                    auto open_positions = futures_query.get_open_positions();
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
            auto syms = result.selected_symbols();
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

    // ---- 5.8. Динамическая ротация при простое pipeline ----
    // В ручном режиме (Manual) ротация отключена — всегда торгуем заданными символами.
    // В автоматическом режиме: фоновый поток проверяет каждые 5 минут: если ВСЕ pipeline простаивают
    // (нет торговли > 30 мин) и нет открытых позиций — пересканировать рынок
    // и заменить pipeline на новые символы.
    const bool rotation_enabled = (config.pair_selection.mode != tb::config::PairSelectionMode::Manual);
    constexpr int64_t kIdleThresholdNs = 10LL * 60 * 1'000'000'000LL;  // 10 мин простоя перед ротацией
    constexpr int kIdleCheckIntervalSec = 120;                          // 2 мин между проверками
    constexpr int64_t kMinRescanIntervalNs = 10LL * 60 * 1'000'000'000LL; // 10 мин между ресканами

    std::atomic<bool> idle_monitor_running{true};
    int64_t last_rescan_ns = comp.clock->now().get();

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
                auto rescan_syms = rescan_v2.selected_symbols();

                if (rescan_syms.empty()) {
                    logger->warn("main", "Ресканирование не нашло подходящих пар");
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
                std::vector<size_t> idle_indices;
                std::unordered_set<std::string> kept_symbols;
                for (size_t i = 0; i < pipelines.size(); ++i) {
                    if (!pipelines[i]->has_open_position() &&
                        pipelines[i]->is_idle(kIdleThresholdNs)) {
                        idle_indices.push_back(i);
                    } else {
                        kept_symbols.insert(active_symbols[i]);
                    }
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
                        ++replaced;
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
    supervisor.wait_for_shutdown();

    // ---- 7. Корректное завершение ----
    logger->info("main", "Начало процедуры завершения работы...");
    idle_monitor_running.store(false);
    if (idle_monitor_thread.joinable()) {
        idle_monitor_thread.join();
    }
    market_scanner->stop_rotation();
    if (metrics_server) {
        metrics_server->stop();
    }
    // Останавливаем pipeline вручную (supervisor может не знать о горячих заменах)
    for (auto& p : pipelines) {
        p->stop();
    }
    supervisor.stop();
    logger->info("main", "Tomorrow Bot завершил работу");

    return 0;
}
