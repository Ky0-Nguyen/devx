add_executable(mpi
  main.cpp
  command_boot.cpp
  command_devices.cpp
  command_apps.cpp
  command_preflight.cpp
  command_analyze.cpp
  command_timeline.cpp
  command_compare.cpp
  command_export.cpp
  command_record.cpp
  command_inspect.cpp
  command_layout.cpp
  command_emulator.cpp
  command_mcp.cpp
  command_sdk_bridge.cpp)
# mpi_core arrives through mpi_adapters, which links it PUBLIC.
#
# mpi_capi is linked for `mpi mcp` alone. The C ABI is the designated
# "one JSON document per call" boundary -- it is what DevX consumes -- and the
# MCP server hands those documents to a model unchanged rather than
# re-summarising a report, which would mean deciding on the model's behalf
# which fields matter. The coverage notes and not-measured markers are exactly
# the fields a summary would drop.
target_link_libraries(mpi PRIVATE mpi_adapters mpi_capi mpi_project_options)
set_target_properties(mpi PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                     "${CMAKE_BINARY_DIR}/bin")
