# The monthly report (reports spec): the snapshot read through the service's read methods and the
# history, its diff against the previous snapshot, the Markdown and HTML rendering, and the
# snapshot and report tables. The job itself runs in svc::ReportComponent on the in-app scheduler.
myclean_add_library(report
  SOURCES
    diff.cpp
    diff.hpp
    month.cpp
    month.hpp
    render.cpp
    render.hpp
    repository.cpp
    repository.hpp
    snapshot.cpp
    snapshot.hpp
  DEPENDS
    myclean::ipc
    myclean::model
    myclean::store
    nlohmann_json::nlohmann_json)
