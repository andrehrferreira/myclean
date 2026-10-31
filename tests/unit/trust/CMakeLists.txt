myclean_add_test(trust
  SOURCES
    kb_data_test.cpp
    trust_test.cpp
  DEPENDS
    myclean::trust)

# The shipped data folder: kb_data_test.cpp loads data/kb/*.json as the service does.
target_compile_definitions(myclean_trust_tests PRIVATE MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
