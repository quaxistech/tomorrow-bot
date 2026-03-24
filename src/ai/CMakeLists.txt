# Библиотека AI Advisory — правиловый анализ и интерфейс ML-сервисов
add_library(tb_ai STATIC ai_advisory_engine.cpp)
target_include_directories(tb_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_ai PUBLIC tb_common tb_features tb_logging)
target_compile_features(tb_ai PUBLIC cxx_std_23)
