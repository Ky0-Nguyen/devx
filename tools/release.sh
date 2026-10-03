#!/bin/bash
# Builds the disk image a release attaches: build/release/DevX-<version>.dmg
# and its .sha256.
#
# Without an identity it produces the same ad-hoc image `devx_dmg` does, so a
# release can still be cut from a machine with no Apple Developer account. With
# one, the bundle is signed with a hardened runtime and a secure timestamp, and
# with notary credentials the image is notarized and stapled, which is what
# removes the right-click > Open step from every recipient's first launch.
#
#   DEVX_SIGN_IDENTITY   "Developer ID Application: Name (TEAMID)". Unset or
#                        "-" signs ad-hoc and skips notarization.
#   DEVX_NOTARY_PROFILE  a keychain profile made once with
#                        `xcrun notarytool store-credentials`, or
#   DEVX_NOTARY_KEY, DEVX_NOTARY_KEY_ID, DEVX_NOTARY_ISSUER
#                        an App Store Connect API key (.p8 path), as CI has.
#   BUILD_DIR            default: build. Must already hold bin/DevX.app.
#
# A Developer ID identity with no notary credentials is refused rather than
# producing an image that is signed but still blocked by Gatekeeper.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT/build}
IDENTITY=${DEVX_SIGN_IDENTITY:--}
APP=$BUILD_DIR/bin/DevX.app
OUT=$BUILD_DIR/release
STAGE=$OUT/stage

[ -d "$APP" ] || { echo "error: $APP not found; build first" >&2; exit 2; }
VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")
DMG=$OUT/DevX-$VERSION.dmg

notary_args=()
if [ -n "${DEVX_NOTARY_PROFILE:-}" ]; then
  notary_args=(--keychain-profile "$DEVX_NOTARY_PROFILE")
elif [ -n "${DEVX_NOTARY_KEY:-}" ]; then
  notary_args=(--key "$DEVX_NOTARY_KEY" --key-id "$DEVX_NOTARY_KEY_ID" --issuer "$DEVX_NOTARY_ISSUER")
fi
if [ "$IDENTITY" != "-" ] && [ ${#notary_args[@]} -eq 0 ]; then
  echo "error: a Developer ID identity needs notary credentials too;" \
       "set DEVX_NOTARY_PROFILE or DEVX_NOTARY_KEY/_KEY_ID/_ISSUER" >&2
  exit 2
fi

rm -rf "$OUT"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/DevX.app"
chmod +x "$STAGE/DevX.app/Contents/MacOS/DevX" "$STAGE/DevX.app/Contents/MacOS/mpi"

# The nested `mpi` is signed on its own before the bundle: notarization checks
# every executable, and `--deep` would sign it without the options below.
if [ "$IDENTITY" = "-" ]; then
  sign=(codesign --force --sign -)
else
  sign=(codesign --force --sign "$IDENTITY" --options runtime --timestamp)
fi
"${sign[@]}" "$STAGE/DevX.app/Contents/MacOS/mpi"
"${sign[@]}" "$STAGE/DevX.app"
codesign --verify --strict --verbose=1 "$STAGE/DevX.app"

ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname DevX -srcfolder "$STAGE" -ov -format UDZO "$DMG"
hdiutil verify -quiet "$DMG"

if [ "$IDENTITY" != "-" ]; then
  codesign --force --sign "$IDENTITY" --timestamp "$DMG"
  xcrun notarytool submit "$DMG" "${notary_args[@]}" --wait
  xcrun stapler staple "$DMG"
  xcrun stapler validate "$DMG"
  spctl --assess --type open --context context:primary-signature --verbose=2 "$DMG"
  echo "signed: $IDENTITY, notarized and stapled"
else
  echo "signed: ad-hoc, not notarized -- recipients must right-click > Open once"
fi

(cd "$OUT" && shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG").sha256")
rm -rf "$STAGE"
echo "version=$VERSION"
echo "dmg=$DMG"
echo "sha256=$(cut -d' ' -f1 "$DMG.sha256")"
