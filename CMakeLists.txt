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

# TA-Lib через кастомный Find-модуль
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FindTALib)

# simdjson
find_package(simdjson QUIET)
if(NOT simdjson_FOUND)
    message(STATUS "simdjson не найден через find_package, ищем вручную...")
    find_library(SIMDJSON_LIBRARIES NAMES simdjson HINTS /usr/local/lib /usr/lib)
    find_path(SIMDJSON_INCLUDE_DIRS NAMES simdjson.h HINTS /usr/local/include /usr/include)
    if(SIMDJSON_LIBRARIES AND SIMDJSON_INCLUDE_DIRS)
        message(STATUS "simdjson найден: ${SIMDJSON_LIBRARIES}")
        add_library(simdjson::simdjson UNKNOWN IMPORTED)
        set_target_properties(simdjson::simdjson PROPERTIES
            IMPORTED_LOCATION "${SIMDJSON_LIBRARIES}"
            INTERFACE_INCLUDE_DIRECTORIES "${SIMDJSON_INCLUDE_DIRS}"
        )
    else()
        message(WARNING "simdjson не найден — некоторые функции могут быть недоступны")
    endif()
endif()

# Включение тестирования
include(CTest)
enable_testing()

# Подсистемы
add_subdirectory(src/common)
add_subdirectory(src/config)
add_subdirectory(src/security)
add_subdirectory(src/logging)
add_subdirectory(src/metrics)
add_subdirectory(src/health)
add_subdirectory(src/clock)
add_subdirectory(src/supervisor)
add_subdirectory(src/platform)
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
add_subdirectory(src/ai)
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
add_subdirectory(src/replay)
add_subdirectory(src/telemetry)
add_subdirectory(src/alpha_decay)
add_subdirectory(src/shadow)
add_subdirectory(src/champion_challenger)
add_subdirectory(src/self_diagnosis)
add_subdirectory(src/adversarial_defense)
add_subdirectory(src/synthetic_scenarios)
add_subdirectory(src/governance)
add_subdirectory(src/operator_control)
add_subdirectory(src/pair_scanner)
add_subdirectory(src/ml)
add_subdirectory(src/pipeline)
add_subdirectory(src/app)

# Тесты
add_subdirectory(tests)

message(STATUS "Tomorrow Bot v${PROJECT_VERSION} — сборка настроена")
