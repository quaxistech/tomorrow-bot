# Библиотека ML-модулей: байесовская адаптация, фильтр энтропии, fingerprinting,
# ликвидационные каскады, корреляции, Thompson Sampling

add_library(tb_ml STATIC
    bayesian_adapter.cpp
    entropy_filter.cpp
    microstructure_fingerprint.cpp
    liquidation_cascade.cpp
    correlation_monitor.cpp
    thompson_sampler.cpp
    meta_label.cpp
    calibration.cpp
    regime_ensemble.cpp
)
target_include_directories(tb_ml PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_ml PUBLIC tb_common tb_features tb_logging)
