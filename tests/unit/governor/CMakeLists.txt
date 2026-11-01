myclean_add_test(governor
  SOURCES
    classifier_test.cpp
    forest_test.cpp
    game_mode_test.cpp
    jobs_test.cpp
    ladder_test.cpp
    ladder_test_support.hpp
    notify_test.cpp
    policy_test.cpp
    pressure_test.cpp
    recovery_test.cpp
    shipped_classes_test.cpp
  DEPENDS
    myclean::governor
    myclean::packs
    myclean::fakes)

# The shipped data folder: policy_test.cpp loads data/governor.toml and shipped_classes_test.cpp
# the process-classes pack, as the service does.
target_compile_definitions(myclean_governor_tests PRIVATE
  MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
  MYCLEAN_VERSION="${PROJECT_VERSION}")
