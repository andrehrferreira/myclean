# Registration smoke test on a message-only window. It never pumps messages, so no keystroke or
# clipboard change of the machine is ever read; the watchdog's behaviour is tested over fakes in
# tests/unit/input.
myclean_add_test(platform_input
  SOURCES
    input_sinks_test.cpp
  DEPENDS
    myclean::platform_input)
