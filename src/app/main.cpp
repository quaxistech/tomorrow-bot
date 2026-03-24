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
#include "exchange/bitget/bitget_rest_client.hpp"
#include "common/enums.hpp"
#include <iostream>
#include <string>
#include <span>

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

    // Создаём PairScanner
    auto pair_scanner = std::make_shared<tb::pair_scanner::PairScanner>(
        config.pair_selection, scanner_rest_client, logger);

    // Выполняем первичное сканирование
    logger->info("main", "Запуск сканирования торговых пар...");
    auto scan_result = pair_scanner->scan();

    // Определяем символ для торговли
    std::string active_symbol = "BTCUSDT";  // Fallback по умолчанию
    if (!scan_result.selected.empty()) {
        active_symbol = scan_result.selected.front();
        logger->info("main", "Лучшая пара для торговли: " + active_symbol,
            {{"total_scanned", std::to_string(scan_result.total_pairs_found)},
             {"after_filter", std::to_string(scan_result.pairs_after_filter)},
             {"selected_count", std::to_string(scan_result.selected.size())}});
    } else {
        logger->warn("main", "Сканер не нашёл подходящих пар. Используется BTCUSDT по умолчанию");
    }

    // ---- 5. Создание и запуск супервизора ----
    tb::supervisor::Supervisor supervisor{
        comp.health,
        comp.logger,
        comp.metrics,
        comp.clock
    };

    supervisor.install_signal_handlers();

    // ---- 5.5. Создание торгового pipeline для лучшей пары ----
    auto pipeline = std::make_shared<tb::pipeline::TradingPipeline>(
        config, comp.secret_provider, comp.logger, comp.clock, comp.metrics, comp.health,
        active_symbol);

    // Регистрируем pipeline как подсистему supervisor
    supervisor.register_subsystem("trading_pipeline",
        [pipeline]() { return pipeline->start(); },
        [pipeline]() { pipeline->stop(); });

    // ---- 5.6. Запуск ротации пар (каждые N часов) ----
    pair_scanner->start_rotation([&logger](const std::vector<std::string>& new_symbols) {
        // При ротации логируем новые пары (пока без горячей замены pipeline)
        std::string symbols_str;
        for (const auto& s : new_symbols) {
            if (!symbols_str.empty()) symbols_str += ", ";
            symbols_str += s;
        }
        logger->info("main", "Ротация пар завершена. Новые лучшие пары: " + symbols_str,
            {{"count", std::to_string(new_symbols.size())}});
        // TODO: горячая замена символа в pipeline (требует перезапуск подключений)
    });

    try {
        supervisor.start();
    } catch (const std::exception& ex) {
        logger->critical("main", "Критическая ошибка при запуске",
            {{"error", ex.what()}});
        return 2;
    }

    // ---- 6. Ожидание завершения ----
    logger->info("main", "Система запущена. Ожидание сигнала завершения (SIGTERM/SIGINT)...",
        {{"active_pair", active_symbol}});
    supervisor.wait_for_shutdown();

    // ---- 7. Корректное завершение ----
    logger->info("main", "Начало процедуры завершения работы...");
    pair_scanner->stop_rotation();
    supervisor.stop();
    logger->info("main", "Tomorrow Bot завершил работу");

    return 0;
}
