# Building DevX from source

For installing a release, see the [README](../README.md#install).

## Build

Requirements: a C++20 compiler, CMake ≥ 3.24. No third-party libraries
([why](docs/adr/0002-zero-third-party-dependencies.md)), so no network access
is needed to build.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

Run the tests (543 C++ cases in 23 binaries, plus 502 Swift checks and 78 smoke checks -- read these from the binaries rather than trusting this line: `for b in build/bin/test_*; do "$b"; done`, `./build/bin/devx_swift_tests`, `bash tools/smoke-test.sh`):

```bash
cd build && ctest --output-on-failure
```

Build with ASan + UBSan — development only, never for benchmark capture
(spec §15):

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMPI_ENABLE_SANITIZERS=ON
cmake --build build-asan && (cd build-asan && ctest --output-on-failure)
```

Artifacts land in `build/bin/`: `mpi`, `DevX.app`, `devx-serve`. Warnings are
errors by default (`-DMPI_ENABLE_WARNINGS_AS_ERRORS=OFF` to relax).

The SwiftUI app needs macOS with Swift and the macOS SDK; its build target
self-skips when either is missing, so the repository still builds elsewhere.


`cmake --build build --target devx_install` copies `DevX.app` to
`~/Applications` and re-signs it there, so the Dock and Spotlight can find it.
`cmake --build build --target devx_dmg` builds `build/DevX.dmg`, an ad-hoc
signed image. A release image, optionally Developer ID signed and notarized,
comes from `tools/release.sh`; see
[packaging and signing](packaging-and-signing.md). Apple Silicon only,
macOS 14 or newer.

The device prerequisites in the [README](../README.md#before-you-connect-a-device)
apply to a source build too.

## Try it without a device

The fixtures live in this repository, so these need a checkout:

```bash
# A positive fixture: frame misses, a long JS task, a CPU hotspot, tooling cost
./build/bin/mpi analyze fixtures/traces/positive-frames-js-cpu.mpi.json

# A healthy capture: detectors RUN and find nothing (not the same as not running)
./build/bin/mpi analyze fixtures/traces/negative-healthy.mpi.json

# Incomplete evidence: proxy frames, unmapped clock, too few samples, drops
./build/bin/mpi analyze fixtures/traces/incomplete-evidence.mpi.json

# Real third-party formats
./build/bin/mpi analyze fixtures/traces/hermes-profile.json
./build/bin/mpi analyze fixtures/traces/chrome-trace-event.json
```

The three `.mpi.json` fixtures are labelled `synthetic` and their reports say so at the top. The Hermes and Chrome files are third-party formats with no such field, so their reports carry no banner; they exercise the reader, and nothing in them was measured on a device either.

## Releasing

`tools/release.sh` builds `build/release/DevX-<version>.dmg`, and
`.github/workflows/release.yml` runs it in CI. Both are described in
[packaging and signing](packaging-and-signing.md).

## Further reading

- [Architecture](architecture.md) — layout, data flow, and design notes
- [Packaging, signing and licenses](packaging-and-signing.md)
- [Requirement → test map](requirement-test-map.md) — 177/198 checklist items, with reasons for the rest
- [Milestone report](milestone-report.md) — M0/M1 in the format spec §21 asks for
- [iOS toolchain probe record](capabilities/ios-toolchain-probe.md) — the M0 schema validation
- [Third-party licenses](../THIRD-PARTY-LICENSES.md) — empty, deliberately
