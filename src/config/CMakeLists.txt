# Библиотека конфигурации системы Tomorrow Bot
# Загрузка, валидация, хеширование конфигурационных файлов
# D5: yaml-cpp вместо самописного flat-парсера

find_package(yaml-cpp 0.7 QUIET)
if(NOT yaml-cpp_FOUND)
    find_library(YAML_CPP_LIB yaml-cpp REQUIRED)
    if(NOT YAML_CPP_LIB)
        message(FATAL_ERROR "yaml-cpp library not found (apt install libyaml-cpp-dev)")
    endif()
endif()

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

if(TARGET yaml-cpp::yaml-cpp)
    target_link_libraries(tb_config PRIVATE yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    target_link_libraries(tb_config PRIVATE yaml-cpp)
else()
    target_link_libraries(tb_config PRIVATE ${YAML_CPP_LIB})
endif()

target_compile_features(tb_config PUBLIC cxx_std_23)

add_library(TomorrowBot::Config ALIAS tb_config)
