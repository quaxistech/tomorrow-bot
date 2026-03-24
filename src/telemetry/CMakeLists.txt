# Модуль телеметрии Tomorrow Bot
add_library(tb_telemetry STATIC
    memory_telemetry_sink.cpp
    research_telemetry.cpp
)
target_include_directories(tb_telemetry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_telemetry PUBLIC tb_common)
target_compile_features(tb_telemetry PUBLIC cxx_std_23)
