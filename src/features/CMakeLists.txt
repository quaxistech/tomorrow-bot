add_library(tb_features STATIC feature_engine.cpp advanced_features.cpp microstructure_features.cpp)
target_include_directories(tb_features PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_features PUBLIC
    tb_common tb_logging tb_metrics tb_clock
    tb_normalizer tb_order_book tb_buffers tb_indicators
)
