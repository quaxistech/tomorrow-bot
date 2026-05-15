# Буферы — header-only шаблонная библиотека
add_library(tb_buffers INTERFACE)
target_include_directories(tb_buffers INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_buffers INTERFACE tb_common tb_normalizer)
