# Trust signal of a process image: signature, signer, path class, knowledge-base match.
myclean_add_library(trust
  SOURCES
    trust.cpp
    trust.hpp
  DEPENDS
    myclean::model
    myclean::rules)
