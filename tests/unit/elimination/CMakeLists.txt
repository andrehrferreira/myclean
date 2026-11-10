# Guided freeze elimination over fakes: the rates, the ranked suspects, the pause keeper with a
# fake process control (nothing real is suspended), the PauseSuspect action and input.pauses.
myclean_add_test(elimination
  SOURCES
    elimination_test.cpp
  DEPENDS
    myclean::elimination
    myclean::fakes)
# The temporary folder of the actions tests: "actions/actions_test_support.hpp".
target_include_directories(myclean_elimination_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
