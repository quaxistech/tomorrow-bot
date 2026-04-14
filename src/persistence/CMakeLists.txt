# Модуль персистентности Tomorrow Bot
add_library(tb_persistence STATIC
    memory_storage_adapter.cpp
    event_journal.cpp
    snapshot_store.cpp
    persistence_layer.cpp
    postgres_storage_adapter.cpp
    wal_writer.cpp
)
target_include_directories(tb_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_persistence PUBLIC tb_common tb_logging tb_clock tb_metrics pqxx pq Boost::json)
target_compile_features(tb_persistence PUBLIC cxx_std_23)
