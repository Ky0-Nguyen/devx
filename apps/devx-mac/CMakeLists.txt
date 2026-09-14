# DevX -- the native SwiftUI desktop app.
#
# Built with swiftc rather than xcodebuild so `cmake --build` produces
# everything and there is no .xcodeproj to keep in sync. The Swift code reaches
# the C++ core through the C ABI in core/capi, which is the boundary spec
# section 5 asks for: no C++ exception can cross it.
#
# Skipped automatically when Swift or the macOS SDK is unavailable, so the
# repository still builds on a host without them.

find_program(MPI_SWIFTC swiftc HINTS /usr/bin)
execute_process(COMMAND xcrun --show-sdk-path --sdk macosx
                OUTPUT_VARIABLE MPI_MACOS_SDK
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)

if(NOT APPLE OR NOT MPI_SWIFTC OR NOT MPI_MACOS_SDK)
  message(STATUS "DevX (SwiftUI app): skipped -- needs macOS with Swift and the macOS SDK")
  return()
endif()

# The sanitizer build exists to run the C++ core's tests. A SwiftUI binary
# cannot link against the sanitizer runtime here (SwiftUICore refuses a
# non-allowed client), and spec section 15 keeps sanitizers to development
# tests anyway -- so the app is simply not part of that configuration.
if(MPI_ENABLE_SANITIZERS)
  message(STATUS "DevX (SwiftUI app): skipped in the sanitizer build")
  return()
endif()

message(STATUS "DevX (SwiftUI app): building with ${MPI_SWIFTC}")

set(DEVX_SWIFT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Tab.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Core.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Theme.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/AppState.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Views.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Views2.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/LiveView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/IssuesView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/TimelineModel.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/TimelineView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/CompareModel.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/IssueFilter.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Recents.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/CompareView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/LaunchOptions.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/DevXApp.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/main.swift
)

# The layers a unit test can reach: everything except the @main entry point,
# which cannot coexist with a main.swift test driver.
set(DEVX_TESTABLE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Tab.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Core.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Theme.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/LaunchOptions.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/TimelineModel.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/CompareModel.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/IssueFilter.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Recents.swift
)

set(DEVX_BUNDLE  ${CMAKE_BINARY_DIR}/bin/DevX.app)
set(DEVX_BINARY  ${DEVX_BUNDLE}/Contents/MacOS/DevX)

# The C ABI header is copied next to the modulemap so the module has a stable
# include path regardless of where the build runs from.
configure_file(${CMAKE_SOURCE_DIR}/core/capi/mpi_capi.h
               ${CMAKE_CURRENT_BINARY_DIR}/modules/mpi_capi.h COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Support/module.modulemap
               ${CMAKE_CURRENT_BINARY_DIR}/modules/module.modulemap COPYONLY)

add_custom_command(
  OUTPUT ${DEVX_BINARY}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DEVX_BUNDLE}/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DEVX_BUNDLE}/Contents/Resources
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/Support/Info.plist
          ${DEVX_BUNDLE}/Contents/Info.plist
  COMMAND ${MPI_SWIFTC}
          -O -whole-module-optimization
          -sdk ${MPI_MACOS_SDK}
          -target arm64-apple-macos14.0
          -module-name DevX
          -I ${CMAKE_CURRENT_BINARY_DIR}/modules
          -framework SwiftUI -framework AppKit
          -L $<TARGET_FILE_DIR:mpi_capi>
          -L $<TARGET_FILE_DIR:mpi_core>
          -L $<TARGET_FILE_DIR:mpi_adapters>
          $<TARGET_FILE:mpi_capi> $<TARGET_FILE:mpi_adapters> $<TARGET_FILE:mpi_core>
          -lc++
          ${DEVX_SWIFT_SOURCES}
          -o ${DEVX_BINARY}
  # An ad-hoc signature is enough for local use and keeps macOS from refusing
  # to launch it. A distributed build needs a real identity; see
  # THIRD-PARTY-LICENSES.md.
  COMMAND codesign --force --sign - ${DEVX_BUNDLE}
  DEPENDS ${DEVX_SWIFT_SOURCES} mpi_capi mpi_core mpi_adapters
          ${CMAKE_CURRENT_SOURCE_DIR}/Support/Info.plist
          ${CMAKE_CURRENT_BINARY_DIR}/modules/mpi_capi.h
          ${CMAKE_CURRENT_BINARY_DIR}/modules/module.modulemap
  COMMENT "Building DevX.app (SwiftUI)"
  VERBATIM
)

add_custom_target(devx_app ALL DEPENDS ${DEVX_BINARY})

# Swift-side tests. Built as a plain executable so ctest can run it without a
# test-bundle host, matching how the C++ tests run. Only the pure layers are
# included: the views need a running app, and a screenshot is not a unit test.
set(DEVX_TEST_BINARY ${CMAKE_BINARY_DIR}/bin/devx_swift_tests)
add_custom_command(
  OUTPUT ${DEVX_TEST_BINARY}
  COMMAND ${MPI_SWIFTC}
          -O -sdk ${MPI_MACOS_SDK}
          -target arm64-apple-macos14.0
          -module-name DevXTests
          -I ${CMAKE_CURRENT_BINARY_DIR}/modules
          -framework SwiftUI -framework AppKit
          $<TARGET_FILE:mpi_capi> $<TARGET_FILE:mpi_adapters> $<TARGET_FILE:mpi_core>
          -lc++
          ${DEVX_TESTABLE_SOURCES}
          ${CMAKE_CURRENT_SOURCE_DIR}/Tests/main.swift
          -o ${DEVX_TEST_BINARY}
  DEPENDS ${DEVX_TESTABLE_SOURCES}
          ${CMAKE_CURRENT_SOURCE_DIR}/Tests/main.swift
          mpi_capi mpi_core mpi_adapters
  COMMENT "Building devx_swift_tests"
  VERBATIM
)
add_custom_target(devx_swift_tests ALL DEPENDS ${DEVX_TEST_BINARY})
if(MPI_BUILD_TESTS)
  add_test(NAME devx_swift_tests COMMAND ${DEVX_TEST_BINARY})
endif()
