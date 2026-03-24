add_library(tb_pair_scanner
    pair_scanner.cpp
    pair_scorer.cpp
)

target_include_directories(tb_pair_scanner PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tb_pair_scanner
    PUBLIC tb_exchange_bitget tb_logging tb_config
    PRIVATE Boost::json
)
