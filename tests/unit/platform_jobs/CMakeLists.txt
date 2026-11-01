# Job objects and process actions against the real OS, acting only on the workload helper the
# tests start (no window, below-normal priority, terminated in teardown). Jobs are named
# Global\MyClean.Job.<class>.<nonce> and released by each test. Pass without elevation; the
# permanent-job test runs only with SeCreatePermanentPrivilege (LocalSystem). A workload blocked
# by the antivirus skips the tests with "blocked by AV".
# A Windows-subsystem program without a window: no console host joins the job beside it.
add_executable(myclean_jobs_workload WIN32 workload.cpp)
target_link_options(myclean_jobs_workload PRIVATE /ENTRY:wmainCRTStartup)
# The workload runs under commit caps of a few hundred MiB: without AddressSanitizer, whose
# shadow memory and allocator would be charged to the job.
get_target_property(_myclean_workload_options myclean_jobs_workload COMPILE_OPTIONS)
list(REMOVE_ITEM _myclean_workload_options /fsanitize=address)
set_target_properties(myclean_jobs_workload PROPERTIES COMPILE_OPTIONS "${_myclean_workload_options}")
_myclean_register_lint_sources(workload.cpp)

myclean_add_test(platform_jobs
  SOURCES
    jobs_test.cpp
    jobs_test_support.hpp
    process_control_test.cpp
    workload_process.cpp
    workload_process.hpp
  DEPENDS
    myclean::platform_jobs
    WIL::WIL)
target_compile_definitions(myclean_platform_jobs_tests PRIVATE
  MYCLEAN_JOBS_WORKLOAD="$<TARGET_FILE:myclean_jobs_workload>")
add_dependencies(myclean_platform_jobs_tests myclean_jobs_workload)
