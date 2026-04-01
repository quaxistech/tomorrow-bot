# Корневой CMake файл проекта Tomorrow Bot
# Адаптивная торговая операционная система для биржи Bitget
cmake_minimum_required(VERSION 3.25)
project(tomorrow-bot VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Экспорт compile_commands.json для IDE и анализаторов
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Подключение модулей CMake
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/ProjectOptions.cmake)

# Поиск внешних зависимостей
find_package(Boost 1.82 REQUIRED COMPONENTS system json thread)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Путь к кастомным CMake-модулям
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Включение тестирования
include(CTest)
enable_testing()

# Подсистемы
add_subdirectory(src/common)
add_subdirectory(src/config)
add_subdirectory(src/security)
add_subdirectory(src/logging)
add_subdirectory(src/metrics)
add_subdirectory(src/clock)
add_subdirectory(src/supervisor)
add_subdirectory(src/exchange)
add_subdirectory(src/normalizer)
add_subdirectory(src/order_book)
add_subdirectory(src/buffers)
add_subdirectory(src/indicators)
add_subdirectory(src/features)
add_subdirectory(src/market_data)
add_subdirectory(src/world_model)
add_subdirectory(src/regime)
add_subdirectory(src/uncertainty)
add_subdirectory(src/strategy)
add_subdirectory(src/strategy_allocator)
add_subdirectory(src/decision)
add_subdirectory(src/execution_alpha)
add_subdirectory(src/opportunity_cost)
add_subdirectory(src/portfolio)
add_subdirectory(src/portfolio_allocator)
add_subdirectory(src/risk)
add_subdirectory(src/execution)
add_subdirectory(src/reconciliation)
add_subdirectory(src/persistence)
add_subdirectory(src/recovery)
add_subdirectory(src/resilience)
add_subdirectory(src/scanner)
add_subdirectory(src/ml)
add_subdirectory(src/leverage)
add_subdirectory(src/pipeline)
add_subdirectory(src/app)

# Тесты
add_subdirectory(tests)

message(STATUS "Tomorrow Bot v${PROJECT_VERSION} — сборка настроена")
