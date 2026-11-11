# The seven-prompt acceptance suite (docs/acceptance.md): each of the owner's prompts of
# docs/prd.md section 7, asked through the MCP server's request handling against one fixture
# machine (the fakes of tests/fakes and tests/unit, the service components on one store and one
# dispatcher), with its pass condition and the audit-log check. Nothing here touches the real PC.
myclean_add_test(acceptance
  SOURCES
    acceptance_fixture.hpp
    audit_check_test.cpp
    prompts_test.cpp
  DEPENDS
    myclean::mcp
    myclean::svc
    myclean::cleaning
    myclean::indexer
    myclean::fakes
    myclean::packs)
target_compile_definitions(myclean_acceptance_tests PRIVATE
  MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
# The fakes of the unit tests: "cleaning/cleaning_fixture.hpp", "svc/phase1_fakes.hpp", ...
target_include_directories(myclean_acceptance_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
