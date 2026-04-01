add_library(tb_scanner
    scanner_engine.cpp
    feature_calculator.cpp
    trap_detectors.cpp
    pair_filter.cpp
    pair_ranker.cpp
    bias_detector.cpp
)

target_include_directories(tb_scanner PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tb_scanner
    PUBLIC tb_exchange_bitget tb_logging tb_metrics
    PRIVATE Boost::json
)
