add_tb_test(test_persistence test_persistence.cpp
    tb_persistence tb_common
)

add_tb_test(test_wal_writer test_wal_writer.cpp
    tb_persistence tb_common tb_logging tb_clock tb_metrics
)
