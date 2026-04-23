add_library(tb_validation STATIC validation_engine.cpp)
target_include_directories(tb_validation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_validation PUBLIC tb_common)
target_compile_features(tb_validation PUBLIC cxx_std_23)
