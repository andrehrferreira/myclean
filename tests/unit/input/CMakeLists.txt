# The input watchdog's chord filter, state machine, classifier and incident store, all over a
# scripted fake probe and FakeClock: no keystroke, window or clipboard of the machine is touched.
myclean_add_test(input
  SOURCES
    chord_filter_test.cpp
    classifier_test.cpp
    fake_probe.hpp
    incident_store_test.cpp
    incident_test.cpp
    watchdog_test.cpp
  DEPENDS
    myclean::input
    myclean::fakes)
