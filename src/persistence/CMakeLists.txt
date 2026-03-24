# Модуль персистентности Tomorrow Bot
add_library(tb_persistence STATIC
    memory_storage_adapter.cpp
    event_journal.cpp
    snapshot_store.cpp
    persistence_layer.cpp
)
target_include_directories(tb_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_persistence PUBLIC tb_common)
target_compile_features(tb_persistence PUBLIC cxx_std_23)
