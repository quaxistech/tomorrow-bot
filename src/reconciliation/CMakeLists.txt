# Модуль reconciliation — сравнение внутреннего состояния с биржей

add_library(tb_reconciliation STATIC
    reconciliation_engine.cpp
)

target_include_directories(tb_reconciliation PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(tb_reconciliation PUBLIC
    tb_common tb_execution tb_portfolio tb_logging tb_clock tb_metrics
)

target_compile_features(tb_reconciliation PUBLIC cxx_std_23)

add_library(TomorrowBot::Reconciliation ALIAS tb_reconciliation)
