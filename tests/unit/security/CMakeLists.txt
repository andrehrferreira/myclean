# The intruder scan's logic (core/security) against in-memory snapshots, a temporary store and
# fake resolvers: persistence points, browser extensions and their origin, verdicts and the
# "suspicious because" reasons, network talkers, reverse DNS and the visible-window log.
myclean_add_test(security
  SOURCES
    extensions_test.cpp
    network_test.cpp
    persistence_test.cpp
    security_fakes.hpp
    verdicts_test.cpp
  DEPENDS
    myclean::security
    myclean::fakes)
