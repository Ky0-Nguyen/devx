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

- **There is no third-party license to ship** beside the binary, and no
  attribution screen to build. DevX's own license is `LICENSE` (MIT). If that changes, the inventory is the thing to update
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
- **It is not notarized.** It is distributed anyway, as a disk image on each
  GitHub release, and every machine it lands on pays the right-click > Open
  step above. Notarization is what would remove that step, and it needs the
  Developer ID this environment does not have.

**Releasing: `tools/release.sh`.** It takes `build/bin/DevX.app` and writes
`build/release/DevX-<version>.dmg` and its `.sha256`. With no identity it
signs ad-hoc, as `devx_dmg` does. With `DEVX_SIGN_IDENTITY` set to a
"Developer ID Application" identity, it signs the nested `mpi` and then the
bundle with a hardened runtime (`--options runtime`) and a secure timestamp,
signs the image, submits it with `xcrun notarytool --wait`, staples it and
runs `spctl --assess`. Notary credentials are either a keychain profile
(`DEVX_NOTARY_PROFILE`, made once with `xcrun notarytool store-credentials`)
or an App Store Connect API key (`DEVX_NOTARY_KEY`, `_KEY_ID`, `_ISSUER`). An
identity without credentials is refused, because an image that is signed but
not notarized is still blocked by Gatekeeper and only looks finished.

The hardened runtime was tested rather than assumed: a bundle signed with
`--options runtime` (an Apple Development identity, on 2026-10-03) launched,
and its Devices tab listed every simulator and the AVD, so `posix_spawnp` of
`xcrun` and the emulator tooling is unaffected. No entitlements are added.
Files reach the app through the open panel, which is the safer default for a
tool that reads whatever path it is handed. `/usr/bin/sample` under the
hardened runtime has not been exercised.

`.github/workflows/release.yml` runs the same script on demand (Actions >
Release, with a tag). It signs and notarizes when the `DEVELOPER_ID_*` and
`NOTARY_*` secrets exist and signs ad-hoc otherwise, attaches the image to the
release, and points `Casks/devx.rb` at it with `tools/bump-cask.sh`, which
also drops the cask's first-launch caveat once a release is notarized.

**What is still missing is the identity.** This machine has an Apple
Development certificate, which can sign but cannot notarize. A Developer ID
Application certificate needs a paid Apple Developer Program membership. Until
then every release is ad-hoc and every recipient clears Gatekeeper by hand
once.

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
