myclean_add_test(rules
  SOURCES
    catalog_test.cpp
    data_files_test.cpp
    fake_data_files.hpp
    parse_classes_test.cpp
    parse_kb_test.cpp
    parse_rules_test.cpp
  DEPENDS
    myclean::rules)

# fixtures/data is a data folder in the shipped layout, read from disk by data_files_test.cpp.
target_compile_definitions(myclean_rules_tests
  PRIVATE MYCLEAN_RULES_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
