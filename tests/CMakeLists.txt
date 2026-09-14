add_library(mpi_test_main STATIC unit/test_framework.cpp)
target_link_libraries(mpi_test_main PUBLIC mpi_core mpi_project_options)

# Each test binary is registered with CTest under its requirement-mapped name so
# the requirement -> test mapping in docs/requirement-test-map.md stays real.
function(mpi_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE mpi_test_main mpi_adapters)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                        "${CMAKE_BINARY_DIR}/bin")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT
                       "MPI_FIXTURE_DIR=${PROJECT_SOURCE_DIR}/fixtures")
endfunction()

mpi_add_test(test_json unit/test_json.cpp)
mpi_add_test(test_xml unit/test_xml.cpp)
mpi_add_test(test_sdk unit/test_sdk.cpp)
mpi_add_test(test_process unit/test_process.cpp)
mpi_add_test(test_identity unit/test_identity.cpp)
mpi_add_test(test_eligibility unit/test_eligibility.cpp)
mpi_add_test(test_trace_model unit/test_trace_model.cpp)
mpi_add_test(test_ingestion unit/test_ingestion.cpp)
mpi_add_test(test_rules unit/test_rules.cpp)
mpi_add_test(test_report unit/test_report.cpp)
mpi_add_test(test_timeline unit/test_timeline.cpp)
mpi_add_test(test_symbols unit/test_symbols.cpp)
mpi_add_test(test_compare unit/test_compare.cpp)
mpi_add_test(test_android_parsers integration/test_android_parsers.cpp)
mpi_add_test(test_ios_parsers integration/test_ios_parsers.cpp)
mpi_add_test(test_android_collector integration/test_android_collector.cpp)

# The HTTP server is in the core now (the SDK ingest endpoint needs it too);
# only the view rendering still lives in the app target.
mpi_add_test(test_devx_http unit/test_devx_http.cpp
             ${PROJECT_SOURCE_DIR}/apps/devx-serve/ui.cpp)
