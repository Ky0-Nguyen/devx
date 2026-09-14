add_executable(mpi main.cpp command_devices.cpp command_apps.cpp
                   command_preflight.cpp command_analyze.cpp
                   command_compare.cpp command_export.cpp command_record.cpp)
target_link_libraries(mpi PRIVATE mpi_core mpi_adapters mpi_project_options)
set_target_properties(mpi PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                     "${CMAKE_BINARY_DIR}/bin")
