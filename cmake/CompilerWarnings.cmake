# Настройка предупреждений компилятора для проекта Tomorrow Bot
# Поддерживаются: GCC и Clang

function(set_project_warnings target_name)
    # Общие предупреждения для всех компиляторов
    set(COMMON_WARNINGS
        -Wall           # Базовые предупреждения
        -Wextra         # Дополнительные предупреждения
        -Wpedantic      # Строгое соответствие стандарту
        -Wshadow                # Скрытие переменных
        -Wnon-virtual-dtor      # Деструкторы без virtual
        -Wcast-align            # Выравнивание при приведении типов
        -Woverloaded-virtual    # Перегрузка виртуальных функций
        -Wsign-conversion       # Знаковые преобразования
        -Wmisleading-indentation # Вводящие в заблуждение отступы
        -Wformat=2              # Форматирование строк
        -Wnull-dereference      # Разыменование nullptr
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(GCC_WARNINGS
            ${COMMON_WARNINGS}
            -Wlogical-op            # Логические операции
            -Wduplicated-branches   # Дублирующиеся ветки
            -Wduplicated-cond       # Дублирующиеся условия
            -Wuseless-cast          # Бесполезные приведения типов
        )
        target_compile_options(${target_name} PRIVATE ${GCC_WARNINGS})
        message(STATUS "GCC предупреждения включены для ${target_name}")

    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(CLANG_WARNINGS
            ${COMMON_WARNINGS}
            -Wimplicit-fallthrough  # Неявный проход через case
        )
        target_compile_options(${target_name} PRIVATE ${CLANG_WARNINGS})
        message(STATUS "Clang предупреждения включены для ${target_name}")
    endif()
endfunction()
