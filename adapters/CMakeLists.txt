add_library(mpi_adapters STATIC
  android/adb_adapter.cpp
  android/adb_collector.cpp
  ios/ios_adapter.cpp
  ios/xctrace_collector.cpp
)
target_link_libraries(mpi_adapters PUBLIC mpi_core mpi_project_options)
