# Библиотека конфигурации системы Tomorrow Bot
# Загрузка, валидация, хеширование конфигурационных файлов

add_library(tb_config STATIC
    config_loader.cpp
    config_validator.cpp
    config_hash.cpp
)

target_include_directories(tb_config
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tb_config
    PUBLIC
        tb_common
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_compile_features(tb_config PUBLIC cxx_std_23)

add_library(TomorrowBot::Config ALIAS tb_config)
