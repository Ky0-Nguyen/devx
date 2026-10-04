cmake_minimum_required(VERSION 3.24)
project(mobile_perf_inspector
        VERSION 0.3.1
        DESCRIPTION "Mobile Performance Inspector -- cross-platform profiler core"
        LANGUAGES CXX)

# Pin the deployment target so the C++ objects and the Swift app agree. Without
# this the C++ defaults to the build host's version and the linker warns on
# every object file when DevX links them.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING
      "Minimum macOS version; must match the SwiftUI app's target" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MPI_ENABLE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(MPI_ENABLE_SANITIZERS "Build with ASan/UBSan (development tests only)" OFF)
option(MPI_BUILD_TESTS "Build the test suite" ON)

add_library(mpi_project_options INTERFACE)
target_compile_options(mpi_project_options INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
  -Wdouble-promotion -Wformat=2
)
if(MPI_ENABLE_WARNINGS_AS_ERRORS)
  target_compile_options(mpi_project_options INTERFACE -Werror)
endif()

# Spec section 15: sanitizers belong in development tests, never in a build
# used for benchmark capture. Guarded behind an explicit opt-in flag.
if(MPI_ENABLE_SANITIZERS)
  target_compile_options(mpi_project_options INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(mpi_project_options INTERFACE
    -fsanitize=address,undefined)
endif()

target_include_directories(mpi_project_options INTERFACE ${PROJECT_SOURCE_DIR})

add_subdirectory(core)
add_subdirectory(adapters)
add_subdirectory(core/capi)
add_subdirectory(apps/cli)
add_subdirectory(apps/devx-serve)
add_subdirectory(apps/devx-mac)

if(MPI_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
