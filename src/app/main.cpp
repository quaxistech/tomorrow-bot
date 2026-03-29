/**
 * @file main.cpp
 * @brief Точка входа приложения Tomorrow Bot
 * 
 * Последовательность инициализации:
 * 1. Парсинг аргументов командной строки
 * 2. Загрузка и валидация конфигурации
 * 3. Инициализация компонентов
 * 4. Сканирование торговых пар (PairScanner)
 * 5. Создание pipeline для каждого выбранного символа
 * 6. Создание и запуск супервизора
 * 7. Запуск ротации пар (каждые 24ч)
 * 8. Ожидание сигнала завершения
 * 9. Корректное завершение работы
 */
#include "app_bootstrap.hpp"
#include "platform/platform_info.hpp"
#include "supervisor/supervisor.hpp"
#include "pipeline/trading_pipeline.hpp"
#include "pair_scanner/pair_scanner.hpp"
#include "common/exchange_rules.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "security/secret_provider.hpp"
#include "common/enums.hpp"
#include <boost/json.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <span>
#include <unordered_map>
#include <thread>
#include <chrono>

namespace {

/// Параметры командной строки
struct CliArgs {
    std::string config_path{"configs/paper.yaml"};  ///< Путь к файлу конфигурации
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
              << "  -c, --config=PATH  Путь к файлу конфигурации (по умолчанию: configs/paper.yaml)\n"
              << "  -v, --version      Показать версию и выйти\n"
              << "  -h, --help         Показать эту справку\n\n"
              << "Режимы торговли (задаются в конфигурации):\n"
              << "  paper      Бумажная торговля (без реальных денег)\n"
              << "  shadow     Теневой режим (расчёты без исполнения)\n"
              << "  testnet    Тестовая сеть биржи\n"
              << "  production Реальная торговля\n";
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
    auto platform = tb::platform::get_platform_info();
    auto banner   = tb::platform::format_startup_banner(platform);
    std::cout << banner;

    logger->info("main", "Запуск Tomorrow Bot",
        {{"config", args.config_path},
         {"mode",   std::string(tb::to_string(config.trading.mode))},
         {"config_hash", config.config_hash}});

    // ---- 4. Сканирование торговых пар (PairScanner) ----
    // Создаём REST-клиент для сканера (публичный, без аутентификации)
    auto scanner_rest_client = std::make_shared<tb::exchange::bitget::BitgetRestClient>(
        config.exchange.endpoint_rest, "", "", "", logger, config.exchange.timeout_ms);

    // Создаём PairScanner (v5: metrics, health, persistence)
    auto pair_scanner = std::make_shared<tb::pair_scanner::PairScanner>(
        config.pair_selection, scanner_rest_client, logger,
        comp.metrics, comp.health, nullptr /* storage */);

    // Выполняем первичное сканирование
    logger->info("main", "Запуск сканирования торговых пар...");
    auto scan_result = pair_scanner->scan();

    // Определяем символы для торговли (до top_n пар из конфига)
    std::vector<std::string> active_symbols;
    if (!scan_result.selected.empty()) {
        active_symbols = scan_result.selected;
    } else {
        active_symbols.push_back("BTCUSDT");
        logger->warn("main", "Сканер не нашёл подходящих пар. Используется BTCUSDT по умолчанию");
    }

    // ---- 4.5. Проверка удерживаемых активов на бирже ----
    // Если есть открытые позиции (ненулевые балансы), принудительно включаем их символы.
    // Это предотвращает "сиротские" позиции, которые никто не отслеживает.
    if (config.trading.mode == tb::TradingMode::Production ||
        config.trading.mode == tb::TradingMode::Testnet) {
        try {
            auto api_key_res = comp.secret_provider->get_secret(tb::security::SecretRef{"BITGET_API_KEY"});
            auto api_secret_res = comp.secret_provider->get_secret(tb::security::SecretRef{"BITGET_API_SECRET"});
            auto passphrase_res = comp.secret_provider->get_secret(tb::security::SecretRef{"BITGET_PASSPHRASE"});

            if (api_key_res && api_secret_res && passphrase_res) {
                auto auth_rest = std::make_shared<tb::exchange::bitget::BitgetRestClient>(
                    config.exchange.endpoint_rest,
                    *api_key_res, *api_secret_res, *passphrase_res,
                    logger, config.exchange.timeout_ms);

                auto resp = auth_rest->get("/api/v2/spot/account/assets");
                if (resp.status_code == 200) {
                    auto root = boost::json::parse(resp.body);
                    auto& data = root.as_object()["data"].as_array();
                    std::vector<std::string> held_symbols;

                    for (auto& asset : data) {
                        auto& obj = asset.as_object();
                        std::string coin(obj["coin"].as_string());
                        double avail = std::stod(std::string(obj["available"].as_string()));
                        if (coin == "USDT" || avail <= 0.0) continue;

                        double usd_val = 0.0;
                        if (obj.contains("usdtValue")) {
                            try { usd_val = std::stod(std::string(obj["usdtValue"].as_string())); }
                            catch (...) { logger->debug("main", "Не удалось разобрать usdtValue", {{"coin", coin}}); }
                        }

                        // Fallback: если usdtValue ненадёжен (0), запрашиваем тикер
                        if (usd_val < 0.01 && avail > 0.0) {
                            std::string sym = coin + "USDT";
                            auto tresp = auth_rest->get("/api/v2/spot/market/tickers?symbol=" + sym);
                            if (tresp.success) {
                                try {
                                    auto tdoc = boost::json::parse(tresp.body);
                                    auto& tobj = tdoc.as_object();
                                    if (tobj.at("code").as_string() == "00000") {
                                        auto& tdata = tobj.at("data").as_array();
                                        if (!tdata.empty()) {
                                            double px = std::stod(std::string(
                                                tdata[0].as_object().at("lastPr").as_string()));
                                            usd_val = avail * px;
                                        }
                                    }
                                } catch (...) { logger->debug("main", "Не удалось получить тикер для оценки баланса", {{"symbol", sym}}); }
                            }
                        }

                        if (usd_val < 0.50) continue;

                        std::string symbol = coin + "USDT";
                        held_symbols.push_back(symbol);

                        bool already_selected = false;
                        for (const auto& s : active_symbols) {
                            if (s == symbol) { already_selected = true; break; }
                        }
                        if (!already_selected) {
                            active_symbols.push_back(symbol);
                            logger->warn("main",
                                "Принудительно добавлен символ с открытой позицией: " + symbol,
                                {{"coin", coin},
                                 {"balance", std::to_string(avail)},
                                 {"usdt_value", std::to_string(usd_val)}});
                        }
                    }
                    if (!held_symbols.empty()) {
                        std::string held_str;
                        for (const auto& s : held_symbols) {
                            if (!held_str.empty()) held_str += ", ";
                            held_str += s;
                        }
                        logger->info("main", "Удерживаемые активы: " + held_str);
                    }
                }
            }
        } catch (const std::exception& e) {
            logger->warn("main", "Не удалось проверить удерживаемые активы",
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

    // Собираем правила инструментов для cada выбранного символа из результатов сканирования
    // (PairScanner уже загрузил quantityPrecision/pricePrecision/minTradeUSDT из exchange info)
    std::unordered_map<std::string, tb::ExchangeSymbolRules> symbol_rules;
    for (const auto& ps : scan_result.ranked_pairs) {
        tb::ExchangeSymbolRules rules;
        rules.symbol = tb::Symbol(ps.symbol);
        rules.quantity_precision = ps.quantity_precision;
        rules.price_precision = ps.price_precision;
        rules.min_trade_usdt = ps.min_trade_usdt;
        symbol_rules[ps.symbol] = rules;
    }

    // ---- 5. Создание и запуск супервизора ----
    tb::supervisor::Supervisor supervisor{
        comp.health,
        comp.logger,
        comp.metrics,
        comp.clock,
        comp.governance
    };

    supervisor.install_signal_handlers();

    // ---- 5.5. Создание торговых pipeline для КАЖДОЙ выбранной пары ----
    std::vector<std::shared_ptr<tb::pipeline::TradingPipeline>> pipelines;
    pipelines.reserve(active_symbols.size());

    for (size_t i = 0; i < active_symbols.size(); ++i) {
        const auto& sym = active_symbols[i];

        auto pipeline = std::make_shared<tb::pipeline::TradingPipeline>(
            config, comp.secret_provider, comp.logger, comp.clock, comp.metrics, comp.health,
            comp.governance, sym);

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

    // ---- 5.6. Запуск ротации пар (каждые N часов) ----
    pair_scanner->start_rotation([&logger](const std::vector<std::string>& new_symbols) {
        std::string symbols_str;
        for (const auto& s : new_symbols) {
            if (!symbols_str.empty()) symbols_str += ", ";
            symbols_str += s;
        }
        logger->info("main", "Плановая ротация завершена. Лучшие пары: " + symbols_str,
            {{"count", std::to_string(new_symbols.size())}});
    });

    try {
        supervisor.start();
    } catch (const std::exception& ex) {
        logger->critical("main", "Критическая ошибка при запуске",
            {{"error", ex.what()}});
        return 2;
    }

    // ---- 5.7. Динамическая ротация при простое pipeline ----
    // Фоновый поток проверяет каждые 5 минут: если ВСЕ pipeline простаивают
    // (нет торговли > 30 мин) и нет открытых позиций — пересканировать рынок
    // и заменить pipeline на новые символы.
    // Профессиональный throttling: максимум 1 ресканирование каждые 30 минут.
    constexpr int64_t kIdleThresholdNs = 30LL * 60 * 1'000'000'000LL;  // 30 мин
    constexpr int kIdleCheckIntervalSec = 300;                          // 5 мин между проверками
    constexpr int64_t kMinRescanIntervalNs = 30LL * 60 * 1'000'000'000LL; // 30 мин между ресканами

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

            // Throttle: не ресканировать чаще чем раз в 30 мин
            int64_t now_ns = comp.clock->now().get();
            if ((now_ns - last_rescan_ns) < kMinRescanIntervalNs) continue;

            // Проверяем: все ли pipeline простаивают и нет ли открытых позиций
            bool all_idle = true;
            bool any_has_position = false;
            for (const auto& p : pipelines) {
                if (p->has_open_position()) {
                    any_has_position = true;
                    all_idle = false;
                    break;
                }
                if (!p->is_idle(kIdleThresholdNs)) {
                    all_idle = false;
                }
            }

            if (!all_idle || any_has_position) continue;

            logger->warn("main", "Все pipeline простаивают > 30 мин. Запуск ресканирования...",
                {{"pipeline_count", std::to_string(pipelines.size())}});

            try {
                auto rescan = pair_scanner->scan();
                if (rescan.selected.empty()) {
                    logger->warn("main", "Ресканирование не нашло подходящих пар");
                    last_rescan_ns = comp.clock->now().get();
                    continue;
                }

                // Проверяем: новые символы отличаются от текущих?
                bool symbols_changed = false;
                if (rescan.selected.size() != active_symbols.size()) {
                    symbols_changed = true;
                } else {
                    for (size_t i = 0; i < rescan.selected.size(); ++i) {
                        if (rescan.selected[i] != active_symbols[i]) {
                            symbols_changed = true;
                            break;
                        }
                    }
                }

                if (!symbols_changed) {
                    logger->info("main", "Ресканирование: пары не изменились, оставляем текущие");
                    last_rescan_ns = comp.clock->now().get();
                    continue;
                }

                // Символы изменились — горячая замена pipeline
                std::string old_str, new_str;
                for (const auto& s : active_symbols) {
                    if (!old_str.empty()) old_str += ", ";
                    old_str += s;
                }
                for (const auto& s : rescan.selected) {
                    if (!new_str.empty()) new_str += ", ";
                    new_str += s;
                }
                logger->warn("main", "Горячая замена пар: [" + old_str + "] → [" + new_str + "]");

                // 1. Останавливаем старые pipeline
                for (auto& p : pipelines) {
                    p->stop();
                }
                pipelines.clear();

                // 2. Обновляем правила инструментов из нового сканирования
                symbol_rules.clear();
                for (const auto& ps : rescan.ranked_pairs) {
                    tb::ExchangeSymbolRules rules;
                    rules.symbol = tb::Symbol(ps.symbol);
                    rules.quantity_precision = ps.quantity_precision;
                    rules.price_precision = ps.price_precision;
                    rules.min_trade_usdt = ps.min_trade_usdt;
                    symbol_rules[ps.symbol] = rules;
                }

                // 3. Обновляем список активных символов
                active_symbols = rescan.selected;

                // 4. Создаём и запускаем новые pipeline
                for (size_t i = 0; i < active_symbols.size(); ++i) {
                    const auto& sym = active_symbols[i];
                    auto pipeline = std::make_shared<tb::pipeline::TradingPipeline>(
                        config, comp.secret_provider, comp.logger, comp.clock,
                        comp.metrics, comp.health, comp.governance, sym);

                    auto rules_it = symbol_rules.find(sym);
                    if (rules_it != symbol_rules.end()) {
                        pipeline->set_exchange_rules(rules_it->second);
                    }
                    pipeline->set_num_pipelines(static_cast<int>(active_symbols.size()));
                    pipeline->start();
                    pipelines.push_back(std::move(pipeline));

                    logger->info("main", "Новый pipeline создан для " + sym,
                        {{"index", std::to_string(i + 1)},
                         {"total", std::to_string(active_symbols.size())}});
                }

                last_rescan_ns = comp.clock->now().get();
                logger->info("main", "Горячая замена завершена",
                    {{"new_count", std::to_string(active_symbols.size())}});

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
    pair_scanner->stop_rotation();
    // Останавливаем pipeline вручную (supervisor может не знать о горячих заменах)
    for (auto& p : pipelines) {
        p->stop();
    }
    supervisor.stop();
    logger->info("main", "Tomorrow Bot завершил работу");

    return 0;
}
