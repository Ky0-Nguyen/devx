add_library(mpi_adapters STATIC
  android/adb_adapter.cpp
  android/adb_collector.cpp
  android/atrace_parser.cpp
  android/hprof_parser.cpp
  ios/ios_adapter.cpp
  ios/xctrace_collector.cpp
  ios/simulator_host_collector.cpp
  ios/sample_parser.cpp
  ios/view_probe_client.cpp
  android/view_dump.cpp
  layout/layout_capture.cpp
  rn/inspector.cpp
)
target_link_libraries(mpi_adapters PUBLIC mpi_core mpi_project_options)

# The layout probe: a dylib built for the iOS simulator, which `mpi layout`
# injects into an app it launches there (see ios/view_probe/). It is a
# separate binary for a separate platform, so it is built with the simulator
# SDK's clang rather than as part of the host's targets, and skipped when that
# SDK is missing -- `mpi layout` then says the probe was not built.
execute_process(COMMAND xcrun --sdk iphonesimulator --show-sdk-path
                OUTPUT_VARIABLE MPI_IOS_SIM_SDK
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(APPLE AND MPI_IOS_SIM_SDK AND NOT MPI_ENABLE_SANITIZERS)
  set(MPI_VIEW_PROBE ${CMAKE_BINARY_DIR}/bin/libdevx_view_probe.dylib
      CACHE INTERNAL "The iOS simulator layout probe")
  set(MPI_VIEW_PROBE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/ios/view_probe/devx_view_probe.mm)
  add_custom_command(
    OUTPUT ${MPI_VIEW_PROBE}
    COMMAND xcrun --sdk iphonesimulator clang++ -std=c++20 -fobjc-arc -O2
            -Wall -Wextra -Werror
            -target arm64-apple-ios17.0-simulator -dynamiclib
            -framework UIKit -framework Foundation -framework CoreGraphics
            ${MPI_VIEW_PROBE_SRC} -o ${MPI_VIEW_PROBE}
    # The simulator refuses to load unsigned code on Apple Silicon.
    COMMAND codesign --force --sign - ${MPI_VIEW_PROBE}
    DEPENDS ${MPI_VIEW_PROBE_SRC}
    COMMENT "Building the iOS simulator layout probe"
    VERBATIM)
  add_custom_target(devx_view_probe ALL DEPENDS ${MPI_VIEW_PROBE})
  message(STATUS "Layout probe: building for the iOS simulator")
else()
  set(MPI_VIEW_PROBE "" CACHE INTERNAL "The iOS simulator layout probe")
  message(STATUS "Layout probe: skipped -- needs the iOS simulator SDK")
endif()
