# The C ABI is its own static library so a UI can link it without pulling in
# the CLI, and so the boundary stays visible in the build graph.
add_library(mpi_capi STATIC mpi_capi.cpp)
target_link_libraries(mpi_capi PUBLIC mpi_core mpi_adapters mpi_project_options)
