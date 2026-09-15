add_library(mpi_adapters STATIC
  android/adb_adapter.cpp
  android/adb_collector.cpp
  android/atrace_parser.cpp
  android/hprof_parser.cpp
  ios/ios_adapter.cpp
  ios/xctrace_collector.cpp
  ios/simulator_host_collector.cpp
  ios/sample_parser.cpp
  rn/inspector.cpp
)
target_link_libraries(mpi_adapters PUBLIC mpi_core mpi_project_options)
