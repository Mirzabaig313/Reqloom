#!/usr/bin/env bash
# check-version.sh <tag> <repo-root>
# A release tag must carry the version in CMakeLists.txt, which is the one place
# the CLI and the desktop app read it from. Prints that version and exits 0 for
# v0.1.2, test-v0.1.2 or test3-v0.1.2 when CMakeLists.txt says 0.1.2; otherwise
# explains the mismatch and exits 1.
set -uo pipefail

tag="${1:-}"
root="${2:-.}"

declared="$(sed -nE 's/^[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+)[[:space:]]*$/\1/p' "$root/CMakeLists.txt" | head -n 1)"
if [[ -z "$declared" ]]; then
  echo "no project VERSION found in $root/CMakeLists.txt" >&2
  exit 1
fi

if [[ ! "$tag" =~ ^(test[0-9]*-)?v([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
  echo "tag '$tag' is not v<major>.<minor>.<patch> (or test-v…); CMakeLists.txt says $declared" >&2
  exit 1
fi
tagged="${BASH_REMATCH[2]}"

if [[ "$tagged" != "$declared" ]]; then
  echo "tag $tag is version $tagged but CMakeLists.txt says $declared" >&2
  exit 1
fi
echo "$declared"
