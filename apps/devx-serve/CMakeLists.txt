# devx-serve -- the headless/remote alternative to DevX.app.
#
# DevX.app (apps/devx-mac) is the desktop application. This serves the same
# views over loopback HTTP, which is what you want when the host has no
# SwiftUI: a Linux or Windows machine driving the Android workflow, or a
# session reached over SSH.
add_executable(devx-serve main.cpp ui.cpp)
# mpi_adapters links mpi_core PUBLIC, so naming it here too put the archive
# on the link line twice and ld warned about the duplicate.
target_link_libraries(devx-serve PRIVATE mpi_adapters mpi_project_options)
set_target_properties(devx-serve PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                      "${CMAKE_BINARY_DIR}/bin")
