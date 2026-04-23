add_tb_test(test_ml ml_test.cpp
    tb_ml tb_features tb_regime tb_logging tb_common
)

add_tb_test(test_meta_label test_meta_label.cpp
    tb_ml tb_common tb_logging
)

add_tb_test(test_calibration test_calibration.cpp
    tb_ml tb_common
)

add_tb_test(test_regime_ensemble test_regime_ensemble.cpp
    tb_ml tb_regime tb_common
)
