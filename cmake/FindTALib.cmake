# Поиск библиотеки TA-Lib (Technical Analysis Library)
# TA-Lib должна быть уже установлена в системе — НЕ собирается из исходников
# Устанавливает: TALIB_FOUND, TALIB_INCLUDE_DIRS, TALIB_LIBRARIES
# Создаёт импортированную цель: TomorrowBot::TALib
#
# Если TA-Lib не найдена — выдаётся предупреждение (не ошибка).
# Модуль индикаторов (Phase 2) будет недоступен без TA-Lib.

include(FindPackageHandleStandardArgs)

# Поиск заголовочного файла
find_path(TALIB_INCLUDE_DIRS
    NAMES ta-lib/ta_libc.h ta_libc.h
    HINTS
        /usr/local/include
        /usr/include
        /opt/ta-lib/include
        /usr/local/include/ta-lib
    DOC "Директория заголовков TA-Lib"
)

# Поиск разделяемой или статической библиотеки
find_library(TALIB_LIBRARIES
    NAMES ta_lib ta-lib libta_lib libta-lib
    HINTS
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /opt/ta-lib/lib
    DOC "Файл библиотеки TA-Lib"
)

# Стандартная обработка аргументов (не REQUIRED — опциональная зависимость)
find_package_handle_standard_args(TALib
    DEFAULT_MSG
    TALIB_LIBRARIES TALIB_INCLUDE_DIRS
)

if(TALIB_FOUND)
    message(STATUS "TA-Lib найдена: ${TALIB_LIBRARIES}")
    message(STATUS "TA-Lib заголовки: ${TALIB_INCLUDE_DIRS}")

    # Создание импортированной цели
    if(NOT TARGET TomorrowBot::TALib)
        add_library(TomorrowBot::TALib UNKNOWN IMPORTED)
        set_target_properties(TomorrowBot::TALib PROPERTIES
            IMPORTED_LOCATION "${TALIB_LIBRARIES}"
            INTERFACE_INCLUDE_DIRECTORIES "${TALIB_INCLUDE_DIRS}"
        )
    endif()
else()
    message(WARNING
        "TA-Lib не найдена — модуль индикаторов (Phase 2) будет недоступен.\n"
        "Установите TA-Lib: https://ta-lib.org/ или sudo apt install libta-lib-dev\n"
        "Сборка продолжается без TA-Lib."
    )
    # Создаём заглушку цели для сборки без TA-Lib
    if(NOT TARGET TomorrowBot::TALib)
        add_library(TomorrowBot::TALib INTERFACE IMPORTED)
    endif()
endif()

mark_as_advanced(TALIB_INCLUDE_DIRS TALIB_LIBRARIES)
