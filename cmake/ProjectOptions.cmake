# Общие опции сборки проекта Tomorrow Bot
# LTO, оптимизации, assert'ы стандартной библиотеки

# Включение Link Time Optimization для Release-сборок
option(ENABLE_LTO "Включить Link Time Optimization" OFF)

if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
    if(lto_supported)
        message(STATUS "LTO включён")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "LTO не поддерживается: ${lto_error}")
    endif()
endif()

# Строгие assert'ы стандартной библиотеки в Debug-режиме
function(set_project_options target_name)
    target_compile_options(${target_name} PRIVATE
        $<$<CONFIG:Debug>:-O0 -g3>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
    )

    # Включаем проверки libstdc++ в Debug-режиме
    target_compile_definitions(${target_name} PRIVATE
        $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS=1>
    )
endfunction()
