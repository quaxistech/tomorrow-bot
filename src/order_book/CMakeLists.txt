add_library(tb_order_book STATIC order_book.cpp)
target_include_directories(tb_order_book PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_order_book PUBLIC 
    tb_common tb_logging tb_metrics tb_normalizer
)
