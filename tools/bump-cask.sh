#!/bin/bash
# Points Casks/devx.rb at a release: bump-cask.sh <version> <sha256> [notarized]
# With "notarized" the first-launch caveat is removed, because it would be
# telling people to work around a Gatekeeper refusal that no longer happens.
set -euo pipefail
cask=$(cd "$(dirname "$0")/.." && pwd)/Casks/devx.rb
version=$1 sha=$2
sed -i.bak -E "s/^  version \".*\"/  version \"$version\"/; s/^  sha256 \".*\"/  sha256 \"$sha\"/" "$cask"
if [ "${3:-}" = notarized ]; then
  sed -i.bak -E '/^  caveats <<~EOS$/,/^  EOS$/d' "$cask"
  sed -i.bak -e ':a' -e '/^\n*$/{$d;N;ba' -e '}' "$cask"
fi
rm -f "$cask.bak"
grep -E '^  (version|sha256) ' "$cask"
