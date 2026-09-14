# Third-party license inventory

## Runtime and build dependencies: none

The core library, the platform adapters and the `mpi` CLI link against no
third-party code. JSON parsing and serialization, the child-process runner, the
source-map VLQ decoder, the R8 mapping reader and the test framework are all
implemented in this repository. See ADR-0002 for the reasoning.

This inventory is therefore **empty**, which is deliberate and is the state to
preserve as the project grows.

| Component | Source | License |
|---|---|---|
| (none) | -- | -- |

## Build-time tools (not redistributed, not linked)

These are required to build but no part of them ends up in the binary:

| Tool | Used for | License |
|---|---|---|
| CMake >= 3.24 | build configuration | BSD-3-Clause |
| Ninja (optional) | build execution | Apache-2.0 |
| Apple Clang / libc++ | compiler and standard library | Apple/LLVM (Apache-2.0 with LLVM exceptions) |
| Python 3 (optional) | fixture and documentation generation in `tools/` | PSF |

## Platform tools invoked at runtime (not redistributed)

The adapters execute these as separate processes. None is bundled, linked or
redistributed; each must already be installed by the user.

| Tool | Platform | Provided by |
|---|---|---|
| `adb` | Android | Android SDK Platform Tools (Apache-2.0), installed by the user |
| `xcrun`, `devicectl`, `simctl`, `xctrace`, `plutil` | iOS | Apple Xcode, installed by the user under Apple's licence |

## Obligations if this changes

Before adding any dependency:

1. Record it in the table above with its exact version and licence.
2. For a desktop UI framework in particular, review the redistribution terms.
   Spec section 5 calls out Qt specifically: Qt 6 under LGPL-3.0 imposes
   relinking and notice obligations on a distributed binary, and the commercial
   licence is an alternative. ADR-0004 keeps that decision open.
3. Confirm the dependency does not introduce an unbounded parser on an
   untrusted input path, which would undermine the limits in `json::Limits`.
