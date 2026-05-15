# Основное приложение Tomorrow Bot

add_executable(tomorrow-bot
    main.cpp
    app_bootstrap.cpp
    http_server.cpp
    runtime_manifest.cpp
)

target_include_directories(tomorrow-bot
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tomorrow-bot
    PRIVATE
        Boost::json
        Boost::system
        tb_pipeline
        tb_scanner
        tb_exchange_bitget
        tb_common
        tb_config
        tb_security
        tb_logging
        tb_metrics
        tb_clock
        tb_supervisor
        Threads::Threads
)

# Передаём версию приложения через define
target_compile_definitions(tomorrow-bot
    PRIVATE
        TB_VERSION="${PROJECT_VERSION}"
)

target_compile_features(tomorrow-bot PRIVATE cxx_std_23)

# Устанавливаем исполняемый файл
install(TARGETS tomorrow-bot
    RUNTIME DESTINATION bin
)
