# Общие типы и утилиты системы Tomorrow Bot (интерфейсная библиотека)
# Только заголовочные файлы — не требует компиляции

add_library(tb_common INTERFACE)

target_include_directories(tb_common
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/..  # Позволяет #include "common/types.hpp"
)

target_compile_features(tb_common
    INTERFACE cxx_std_20
)

# Псевдоним для удобства использования в других модулях
add_library(TomorrowBot::Common ALIAS tb_common)
