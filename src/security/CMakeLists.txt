# Библиотека безопасности системы Tomorrow Bot
# Управление секретами, маскирование, учётные данные

add_library(tb_security STATIC
    secret_provider.cpp
    redaction.cpp
    production_guard.cpp
)

target_include_directories(tb_security
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tb_security
    PUBLIC
        tb_common
)

target_compile_features(tb_security PUBLIC cxx_std_20)

add_library(TomorrowBot::Security ALIAS tb_security)
