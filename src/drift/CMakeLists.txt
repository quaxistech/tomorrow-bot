add_library(tb_drift STATIC drift_monitor.cpp)
target_include_directories(tb_drift PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_drift PUBLIC tb_common)
target_compile_features(tb_drift PUBLIC cxx_std_23)
