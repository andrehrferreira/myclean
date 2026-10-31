myclean_add_test(trust
  SOURCES
    kb_data_test.cpp
    trust_test.cpp
  DEPENDS
    myclean::packs
    myclean::trust)

# The shipped data folder: kb_data_test.cpp loads the knowledge base of data/packs as the service does.
target_compile_definitions(myclean_trust_tests PRIVATE MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
