# core/clean IGit through the git command line, and the hidden process runner it uses.
myclean_add_library(platform_git
  SOURCES
    git_cli.cpp
    git_cli.hpp
  DEPENDS
    myclean::backup
    myclean::clean
    WIL::WIL)
