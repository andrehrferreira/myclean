# The governor in the service over in-memory fakes: FakeJobs for job objects and process control,
# a FakeClock, a telemetry view and metrics set by the test, and a store in a temporary folder.
# Nothing here touches a real process or job object.
myclean_add_test(governing
  SOURCES
    games_test.cpp
    governing_test_support.hpp
    jobs_test.cpp
    ladder_test.cpp
    methods_test.cpp
    orphans_test.cpp
    records_test.cpp
    recovery_test.cpp
  DEPENDS
    myclean::governing
    myclean::fakes)
# The fake GPU counters: "telemetry/fake_sources.hpp".
target_include_directories(myclean_governing_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
