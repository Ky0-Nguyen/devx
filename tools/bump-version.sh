#!/bin/bash
# Bumps the version in the one place it is written: project(VERSION) in the
# top-level CMakeLists.txt. Info.plist, `mpi`, the MCP server and session
# manifests all take it from there at build time.
#
#   bump-version.sh patch|minor|major    -> prints the new version
#   bump-version.sh 1.2.3                -> sets exactly that
#   bump-version.sh --current            -> prints the current one
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
file="$root/CMakeLists.txt"
current=$(sed -nE 's/^[[:space:]]*VERSION ([0-9]+\.[0-9]+\.[0-9]+)[[:space:]]*$/\1/p' "$file" | head -1)
[ -n "$current" ] || { echo "error: no 'VERSION x.y.z' line in $file" >&2; exit 2; }
if [ "${1:-}" = "--current" ]; then echo "$current"; exit 0; fi

IFS=. read -r major minor patch <<<"$current"
case "${1:-patch}" in
  patch) next="$major.$minor.$((patch + 1))" ;;
  minor) next="$major.$((minor + 1)).0" ;;
  major) next="$((major + 1)).0.0" ;;
  [0-9]*.[0-9]*.[0-9]*) next="$1" ;;
  *) echo "error: expected patch, minor, major or x.y.z, got '$1'" >&2; exit 2 ;;
esac
sed -i.bak -E "s/^([[:space:]]*VERSION )$current([[:space:]]*)$/\1$next\2/" "$file"
rm -f "$file.bak"
echo "$next"
