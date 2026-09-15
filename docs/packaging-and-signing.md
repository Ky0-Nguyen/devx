# Packaging, signing and licenses

What spec J12 asks to be documented, and the honest state of each.

---

## Licenses: the inventory is empty on purpose

`THIRD-PARTY-LICENSES.md` is the authority and it lists nothing, because the
core, the adapters and the CLI link against no third-party code (ADR-0002).
JSON, XML, the process runner, the HTTP server, the source-map VLQ decoder,
the R8 mapping reader, the HPROF parser and the test framework are all in this
repository.

Two consequences worth stating rather than discovering:

- **There is no license file to ship** beside the binary, and no attribution
  screen to build. If that changes, the inventory is the thing to update
  first, not last.
- **Nothing needs network access to build.** `cmake && cmake --build` is the
  whole of it.

The Apple frameworks DevX links (SwiftUI, AppKit, Foundation) are platform
frameworks: linked, not redistributed, and covered by the OS license.

---

## Packaging: what CMake produces

```
build/bin/mpi                     the CLI: one self-contained executable
build/bin/DevX.app                the desktop app
  Contents/Info.plist             from apps/devx-mac/Support/Info.plist
  Contents/MacOS/DevX             the Swift binary, statically linked against
                                  mpi_capi + mpi_core + mpi_adapters
build/bin/devx-serve              the HTTP view of a session, for a browser
build/bin/devx_swift_tests        the Swift-side tests
```

The app bundle carries one resource: `Contents/Resources/DevX.icns`, and
nothing else -- no fonts, no images, no localisations.

The icon is **generated**, not committed. `tools/gen-icon.swift` draws it
with CoreGraphics and packs it with `iconutil`, both of which ship with
macOS, so it adds no dependency and leaves no binary blob in the tree that
a reviewer cannot read. CMake looks `iconutil` up rather than assuming it:
without it the app still builds, with no icon and a `--` line at configure
time saying so.

The copy into `Contents/Resources` happens **before** `codesign`. A resource
added afterwards leaves `_CodeSignature/CodeResources` describing a bundle
that no longer matches itself, which macOS reports as a damaged app.
The typeface is Menlo, which ships with every macOS, and the palette is in
code (ADR-0002 again -- a bundled font file would be the first third-party
asset in the repository).

**To produce a bundle from a clean checkout:**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
open build/bin/DevX.app
```

To put it where the Dock and Spotlight can find it:

```bash
cmake --build build --target devx_install
```

That copies the bundle to `~/Applications/DevX.app` and re-signs it there.
Both details matter. The destination is the **user's** Applications folder, not
`/Applications`: it needs no privilege escalation, and an un-notarized ad-hoc
build has no business installed system-wide. The re-sign is because the copy is
a different set of files on disk; a bundle carried across without one can trip
Gatekeeper on first launch. Override the destination with
`-DDEVX_INSTALL_DIR=...` if you want it elsewhere.

Beyond that there is no installer, no `.dmg` and no Sparkle-style updater.
Distribution is "copy the `.app`", which is adequate for a tool used by the
team that builds it and is not adequate for anything wider -- see below.

---

## Signing: ad-hoc only, and what that costs

The bundle is signed **ad-hoc**:

```cmake
COMMAND codesign --force --sign - ${DEVX_BUNDLE}
```

`--sign -` means no identity: the signature is a checksum with nobody behind
it. That is enough for the app to run on the machine that built it, and it is
the reason the app can be rebuilt and relaunched without a developer account.

**What ad-hoc signing does not do**, each of which has been met in this
project rather than merely anticipated:

- **It does not survive being copied to another Mac.** Gatekeeper will refuse
  an ad-hoc bundle that arrived by download, and the user has to clear the
  quarantine attribute by hand.
- **It grants no entitlements**, so the app cannot raise a TCC consent
  prompt. This is not theoretical: a run-set path typed into the Compare tab
  that lives in `~/Documents` blocks *inside the read*, forever, because the
  prompt that would unblock it cannot be shown. The app probes a handed-over
  path with a deadline and names the wall instead of hanging (section 5 of
  `known-limitations.md`). Files chosen through the open panel are unaffected:
  picking a file is what grants access to it.
- **It is not notarized**, so it is not distributable outside the machine
  that built it.

**What a distributable build would need**, in order:

1. An Apple Developer ID Application certificate in the keychain, and
   `codesign --sign "Developer ID Application: ..."` in place of `--sign -`.
2. A hardened runtime (`--options runtime`), which is required for
   notarization and which changes what the app may do -- `posix_spawnp` of
   `adb` and `xcrun` still works, but it must be tested rather than assumed.
3. Entitlements for the directories the app reads, or the open-panel-only
   approach kept deliberately. The second is the safer default for a tool
   that reads whatever path it is handed.
4. Notarization (`xcrun notarytool submit`) and stapling.
5. A `.dmg` or `.pkg`, and a decision about updates.

**None of steps 1-5 has been done**, because none can be done in this
environment: there is no Developer ID here. So the honest state is: the bundle
runs where it was built, and shipping it is unstarted work with a known shape
rather than an unknown one.

---

## The CLI needs none of this

`build/bin/mpi` is an ordinary executable: no bundle, no signature, no
entitlements. It is the thing to use in CI, and it can do everything the
desktop app can do -- record, analyze, compare, draw a timeline, manage
suppressions -- which is deliberate (spec section 3: the CLI is reusable and
headless, not a subset).
