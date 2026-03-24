# Функция включения санитайзеров для отладочной сборки Tomorrow Bot
# Address Sanitizer (ASan) — обнаружение ошибок памяти
# Undefined Behavior Sanitizer (UBSan) — неопределённое поведение

function(enable_sanitizers target_name)
    # Санитайзеры поддерживаются только GCC и Clang
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(WARNING "Санитайзеры не поддерживаются компилятором ${CMAKE_CXX_COMPILER_ID}")
        return()
    endif()

    # Флаги Address Sanitizer
    set(ASAN_FLAGS
        -fsanitize=address
        -fno-omit-frame-pointer    # Сохраняем стек вызовов
        -fno-optimize-sibling-calls
    )

    # Флаги Undefined Behavior Sanitizer
    set(UBSAN_FLAGS
        -fsanitize=undefined
        -fsanitize=shift
        -fsanitize=shift-exponent
        -fsanitize=integer-divide-by-zero
        -fsanitize=null
        -fsanitize=signed-integer-overflow
        -fsanitize=bounds
        -fno-sanitize-recover=all  # Завершаем программу при ошибке
    )

    target_compile_options(${target_name} PRIVATE ${ASAN_FLAGS} ${UBSAN_FLAGS})
    target_link_options(${target_name} PRIVATE ${ASAN_FLAGS} ${UBSAN_FLAGS})

    message(STATUS "Санитайзеры (ASan + UBSan) включены для ${target_name}")
endfunction()
