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

The two clients in `core/net` are on that list too: a WebSocket client that
speaks enough of RFC 6455 to reach the inspector a React Native debug build
already runs, and the one-shot HTTP GET that fetches its target list from
Metro. Both are loopback-only, and the WebSocket client's omissions -- binary
frames discarded, no extensions -- are listed in its header rather than
discovered.

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
nothing else -- no fonts, no images, no localisation resources.
(The app *is* translated -- see `docs/internationalisation.md` -- but
the catalog is compiled into the binary rather than shipped as
`.lproj` directories, so the bundle stays as it is described here.)

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

There is also a disk image: `cmake --build build --target devx_dmg` produces `build/DevX.dmg`, described below. There is no `.pkg` installer and no Sparkle-style updater. Distribution is the image around the ad-hoc-signed bundle, which is adequate for a tool used by the team that builds it and by anyone willing to clear Gatekeeper by hand once -- see the next section -- and is not adequate for anything wider.
Distribution is "copy the `.app`", which is adequate for a tool used by the
team that builds it and is not adequate for anything wider -- see below.

---

**The disk image.** `cmake --build build --target devx_dmg` produces
`build/DevX.dmg` -- in the build directory, not `build/bin`. It is not part
of the default build; a release runs it and attaches the result. It is a
CMake target rather than a remembered `hdiutil` line so the artifact people
download is produced the same way every time and the recipe can be read.

The image is built from a staging directory, `build/dmg-stage`, not from the
bundle directly: `hdiutil create -srcfolder` pointed at the `.app` gives an
image whose only item is the app with no room beside it, and the convention a
Mac user expects is a window they drag *from*. The staging directory is
emptied first, so nothing an older layout left behind ships. The bundle is
copied in, its binary made executable, re-signed ad-hoc and verified -- the
same three steps `devx_install` takes, for the same reason -- and a symlink
to `/Applications` is placed beside it, which is what makes the drag work.
The format is UDZO, compressed and read-only. `hdiutil verify` runs on the
result, because an image that only builds is not an image that installs.

The bundle inside the image carries the same signature as the one in
`build/bin`: `codesign -dv` on it reports `Signature=adhoc` and no team
identifier. Nothing about the image changes what the next section says.

The image's drag target is `/Applications`, which the `devx_install`
paragraph above argues an un-notarized build has no business in. The two
targets disagree: the image follows the Finder convention and the install
target follows the argument, and which should give has not been decided.

The copy is a replace, not a merge: whatever is at `~/Applications/DevX.app`
is removed first, because a stale file left behind from an older layout would
still be inside the signature's scope. The binary's executable bit is set
explicitly, since `copy_directory` does not preserve it on every CMake
version and an app whose binary is not executable fails to launch with a
message that says nothing useful. After the re-sign, `codesign --verify`
runs, so a signature that did not take fails the target rather than the
first launch.

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

- **It does not survive being copied to another Mac.** Gatekeeper refuses an
  ad-hoc bundle that arrived by download, saying the developer cannot be
  verified -- or, after an unusual copy, that the app is damaged. Neither is
  what is wrong. The user right-clicks the app and chooses Open, once; after
  that it launches normally. `docs/RELEASE-v0.1.0.md` leads with that step.
- **It grants no entitlements**, so the app cannot raise a TCC consent
  prompt. This is not theoretical: a run-set path typed into the Compare tab
  that lives in `~/Documents` blocks *inside the read*, forever, because the
  prompt that would unblock it cannot be shown. The app probes a handed-over
  path with a deadline and names the wall instead of hanging (section 5 of
  `known-limitations.md`). Files chosen through the open panel are unaffected:
  picking a file is what grants access to it.
- **It is not notarized.** It is distributed anyway, as `DevX.dmg` since
  v0.1.0, and every machine it lands on pays the right-click > Open step
  above. Notarization is what would remove that step, and it needs the
  Developer ID this environment does not have.
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
5. A decision about updates. The `.dmg` half of this step is done --
   `devx_dmg`, above -- and it is the one step that needs no identity.

**Steps 1-4 have not been done**, because none can be done in this
environment: there is no Developer ID here. The disk image in step 5 has,
because it needs no identity. So the honest state is: the bundle ships as
v0.1.0 without notarization, every recipient clears Gatekeeper by hand once,
and the four steps that would remove that are work with a known shape rather
than an unknown one.

---

**What a recipient sees.** A downloaded `DevX.dmg` opens, and the app dragged
out of it does not: macOS says the developer cannot be verified -- or, after
an unusual copy, that the app is damaged. Neither is what is wrong; the
signature is ad-hoc and Gatekeeper has nobody to ask. The user right-clicks
the app in Applications and chooses **Open**, once, and it launches normally
from then on. `docs/RELEASE-v0.1.0.md` leads with that step rather than
burying it, because a download that fails to open with "damaged" is the most
common way a tool like this is written off as broken before it has run once.
A reader who would rather not do that builds from source and runs
`devx_install`, which signs the bundle on the machine that will run it.

## The CLI needs none of this

`build/bin/mpi` is an ordinary executable: no bundle, no signature, no
entitlements. It is the thing to use in CI, and it can do everything the
desktop app can do -- record, analyze, compare, draw a timeline, manage
suppressions -- which is deliberate (spec section 3: the CLI is reusable and
headless, not a subset).
