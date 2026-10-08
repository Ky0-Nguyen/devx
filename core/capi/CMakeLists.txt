# The C ABI is its own static library so a UI can link it without pulling in
# the CLI, and so the boundary stays visible in the build graph.
add_library(mpi_capi STATIC mpi_capi.cpp mpi_capi_emulator.cpp mpi_capi_control.cpp mpi_capi_browserstack.cpp
            mpi_capi_intelligence.cpp)
# mpi_core arrives through mpi_adapters, which links it PUBLIC.
target_link_libraries(mpi_capi PUBLIC mpi_adapters mpi_project_options)
