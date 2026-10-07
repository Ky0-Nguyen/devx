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
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/InspectFilter.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/InspectView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/LayoutView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/EmulatorView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/EmulatorScreenView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/BrowserStackPanel.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/AppState+Emulator.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/AppState+AIControl.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/IssuesView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/TimelineModel.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/TimelineView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/CompareModel.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/IssueFilter.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Recents.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/ThreadsModel.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Strings.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/DeviceFreshness.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/DeviceFilter.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/BodyFormat.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/LogOrder.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/FindingSelection.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/CurlCommand.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/BodySearch.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/JSONTree.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/McpSetup.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/HelpTopics.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/CounterFormat.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/DisclosureSection.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/JSONTreeView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/HelpView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/McpInstall.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/InspectSelection.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/InspectSecrets.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/ThreadsView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/CompareView.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/SettingsView.swift
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
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/ThreadsModel.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/Strings.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/DeviceFreshness.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/InspectFilter.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/DeviceFilter.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/BodyFormat.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/LogOrder.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/FindingSelection.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/CurlCommand.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/BodySearch.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/JSONTree.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/McpSetup.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/HelpTopics.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/CounterFormat.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/InspectSelection.swift
  ${CMAKE_CURRENT_SOURCE_DIR}/DevX/InspectSecrets.swift
)

set(DEVX_BUNDLE  ${CMAKE_BINARY_DIR}/bin/DevX.app)
set(DEVX_BINARY  ${DEVX_BUNDLE}/Contents/MacOS/DevX)

# The C ABI header is copied next to the modulemap so the module has a stable
# include path regardless of where the build runs from.
configure_file(${CMAKE_SOURCE_DIR}/core/capi/mpi_capi.h
               ${CMAKE_CURRENT_BINARY_DIR}/modules/mpi_capi.h COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Support/module.modulemap
               ${CMAKE_CURRENT_BINARY_DIR}/modules/module.modulemap COPYONLY)

# The app icon.
#
# Drawn by tools/gen-icon.swift at build time rather than committed as an
# .icns, which keeps the promise ADR-0002 makes about this tree: no binary
# asset that a reviewer cannot read. `iconutil` ships with macOS, so this adds
# no dependency -- but it is looked up rather than assumed, and the bundle
# simply goes without an icon if it is missing, the same way the whole app is
# skipped without Swift.
find_program(MPI_ICONUTIL iconutil HINTS /usr/bin)
if(MPI_ICONUTIL)
  set(DEVX_ICON      ${CMAKE_CURRENT_BINARY_DIR}/DevX.icns)
  set(DEVX_ICON_TOOL ${CMAKE_CURRENT_BINARY_DIR}/gen-icon)
  add_custom_command(
    OUTPUT ${DEVX_ICON_TOOL}
    COMMAND ${MPI_SWIFTC} -O -sdk ${MPI_MACOS_SDK}
            -target arm64-apple-macos14.0
            ${CMAKE_SOURCE_DIR}/tools/gen-icon.swift -o ${DEVX_ICON_TOOL}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen-icon.swift
    COMMENT "Building the DevX icon generator"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${DEVX_ICON}
    COMMAND ${DEVX_ICON_TOOL} ${DEVX_ICON}
    DEPENDS ${DEVX_ICON_TOOL}
    COMMENT "Drawing DevX.icns"
    VERBATIM
  )
  # Placed before the signature: codesign hashes the bundle's resources, so an
  # icon copied in afterwards would leave CodeResources describing a bundle
  # that no longer matches itself.
  set(DEVX_ICON_STEP COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     ${DEVX_ICON} ${DEVX_BUNDLE}/Contents/Resources/DevX.icns)
  set(DEVX_ICON_DEP  ${DEVX_ICON})
else()
  message(STATUS "DevX (SwiftUI app): no iconutil -- the bundle will have no icon")
  set(DEVX_ICON_STEP "")
  set(DEVX_ICON_DEP  "")
endif()

# The iOS simulator layout probe rides in Resources, where `mpi layout` looks
# for it beside itself -- including through the Homebrew link, which resolves
# into this bundle. Before the signature, for the same reason as the icon.
if(MPI_VIEW_PROBE)
  set(DEVX_PROBE_STEP COMMAND ${CMAKE_COMMAND} -E copy_if_different
                      ${MPI_VIEW_PROBE} ${DEVX_BUNDLE}/Contents/Resources/libdevx_view_probe.dylib)
  set(DEVX_PROBE_DEP devx_view_probe ${MPI_VIEW_PROBE})
else()
  set(DEVX_PROBE_STEP "")
  set(DEVX_PROBE_DEP "")
endif()

# Info.plist carries the version from project(VERSION), so a release bumps one
# line. CFBundleVersion has to increase from one release to the next as a
# number: 0.3.1 becomes 301, which sorts above the 3 that 0.3.0 shipped with.
math(EXPR DEVX_BUILD_NUMBER
     "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Support/Info.plist.in
               ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

add_custom_command(
  OUTPUT ${DEVX_BINARY}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DEVX_BUNDLE}/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DEVX_BUNDLE}/Contents/Resources
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
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
  ${DEVX_ICON_STEP}
  # The CLI ships inside the bundle, beside the app's own binary.
  #
  # It is what the MCP server is: a host launches `mpi mcp` as a subprocess,
  # and the one thing people get wrong is the path -- a host runs it without a
  # shell, so `mpi` on a PATH is not `mpi` on its PATH. An app that explains
  # how to connect it has to state a path that is true, and the only path it
  # can be certain of is one inside itself. It also keeps the instructions
  # working when the build tree moves.
  #
  # Before the signature, like the icon: codesign hashes what is in the
  # bundle, so a binary added afterwards leaves a signature that no longer
  # matches it.
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:mpi> ${DEVX_BUNDLE}/Contents/MacOS/mpi
  ${DEVX_PROBE_STEP}
  # An ad-hoc signature is enough for local use and keeps macOS from refusing
  # to launch it. A distributed build needs a real identity; see
  # THIRD-PARTY-LICENSES.md.
  COMMAND codesign --force --sign - ${DEVX_BUNDLE}
  DEPENDS ${DEVX_SWIFT_SOURCES} mpi_capi mpi_core mpi_adapters mpi
          ${DEVX_ICON_DEP} ${DEVX_PROBE_DEP}
          ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
          ${CMAKE_CURRENT_BINARY_DIR}/modules/mpi_capi.h
          ${CMAKE_CURRENT_BINARY_DIR}/modules/module.modulemap
  COMMENT "Building DevX.app (SwiftUI)"
  VERBATIM
)

add_custom_target(devx_app ALL DEPENDS ${DEVX_BINARY})

# `cmake --build build --target devx_install` puts the bundle where the Dock
# and Spotlight can find it.
#
# It is a target rather than an `install(DIRECTORY ...)` rule because the app
# is assembled by a custom command, and because the destination is deliberately
# a user-owned directory: ~/Applications needs no privilege escalation, and an
# unsigned, un-notarized build (see docs/packaging-and-signing.md) has no
# business in the system-wide /Applications. The copy is a replace, not a
# merge -- a stale file left behind from an older layout would still be inside
# the signature's scope.
set(DEVX_INSTALL_DIR "$ENV{HOME}/Applications" CACHE PATH
    "Where devx_install copies DevX.app")
add_custom_target(devx_install
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DEVX_INSTALL_DIR}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${DEVX_INSTALL_DIR}/DevX.app
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${DEVX_BUNDLE} ${DEVX_INSTALL_DIR}/DevX.app
  # copy_directory does not preserve the executable bit on every CMake
  # version, and an app whose binary is not executable fails to launch with a
  # message that says nothing useful.
  COMMAND chmod +x ${DEVX_INSTALL_DIR}/DevX.app/Contents/MacOS/DevX
  COMMAND chmod +x ${DEVX_INSTALL_DIR}/DevX.app/Contents/MacOS/mpi
  # Re-sign in place: the copy has new inode metadata, and a bundle whose
  # signature no longer verifies is quarantined by Gatekeeper on first launch.
  COMMAND codesign --force --sign - ${DEVX_INSTALL_DIR}/DevX.app
  COMMAND codesign --verify --verbose=1 ${DEVX_INSTALL_DIR}/DevX.app
  DEPENDS ${DEVX_BINARY}
  COMMENT "Installing DevX.app to ${DEVX_INSTALL_DIR}"
  VERBATIM
)

# `cmake --build build --target devx_dmg` builds the disk image a release
# attaches.
#
# A target rather than a hand-run `hdiutil` line, so the artifact people
# download is produced the same way every time and the recipe is reviewable.
#
# `hdiutil create -srcfolder` on a staging directory rather than on the bundle
# directly: pointing it at the .app produces an image whose only item is the
# app with no room beside it, and the convention a Mac user expects is a
# window they drag *from*. The staging directory carries the app plus a
# symlink to /Applications, which is what makes the drag work.
#
# UDZO is the compressed read-only format. It is what every other Mac app
# ships and it keeps the download to about a third of the bundle.
#
# NOT notarized, and deliberately not pretending to be: this is signed
# ad-hoc (`codesign --sign -`), so Gatekeeper will refuse it on first launch
# until the user right-clicks > Open. docs/packaging-and-signing.md has the
# whole story; the release notes have to repeat it, because a download that
# fails to open with "damaged" is the single most common way a tool like this
# gets written off as broken.
set(DEVX_DMG ${CMAKE_BINARY_DIR}/DevX.dmg)
set(DEVX_DMG_STAGE ${CMAKE_BINARY_DIR}/dmg-stage)
add_custom_command(
  OUTPUT ${DEVX_DMG}
  # A stale staging directory would ship whatever an older layout left in it,
  # inside the image people download.
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${DEVX_DMG_STAGE} ${DEVX_DMG}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DEVX_DMG_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${DEVX_BUNDLE} ${DEVX_DMG_STAGE}/DevX.app
  COMMAND chmod +x ${DEVX_DMG_STAGE}/DevX.app/Contents/MacOS/DevX
  COMMAND chmod +x ${DEVX_DMG_STAGE}/DevX.app/Contents/MacOS/mpi
  # Re-signed after the copy, for the same reason devx_install does it: the
  # copy has new inode metadata and an unverifiable signature is quarantined.
  COMMAND codesign --force --sign - ${DEVX_DMG_STAGE}/DevX.app
  COMMAND codesign --verify --verbose=1 ${DEVX_DMG_STAGE}/DevX.app
  COMMAND ${CMAKE_COMMAND} -E create_symlink
          /Applications ${DEVX_DMG_STAGE}/Applications
  COMMAND hdiutil create -quiet -volname DevX -srcfolder ${DEVX_DMG_STAGE}
          -ov -format UDZO ${DEVX_DMG}
  # Proves the image mounts and carries a launchable bundle. An image that
  # only *builds* is not an image that installs, and finding that out from a
  # user is finding it out too late.
  COMMAND hdiutil verify -quiet ${DEVX_DMG}
  DEPENDS ${DEVX_BINARY}
  COMMENT "Building DevX.dmg"
  VERBATIM
)
add_custom_target(devx_dmg DEPENDS ${DEVX_DMG})

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
