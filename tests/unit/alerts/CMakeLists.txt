myclean_add_test(alerts
  SOURCES
    config_test.cpp
    engine_test.cpp
  DEPENDS
    myclean::alerts
    myclean::fakes)

# The shipped data folder: config_test.cpp loads data/alerts.toml as the service does.
target_compile_definitions(myclean_alerts_tests PRIVATE MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
