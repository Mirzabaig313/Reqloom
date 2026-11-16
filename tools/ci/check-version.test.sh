#!/usr/bin/env bash
# Tests tools/ci/check-version.sh against a temporary CMakeLists.txt.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT
printf 'cmake_minimum_required(VERSION 3.28)\nproject(Reqloom\n    VERSION 0.1.2\n    LANGUAGES CXX\n)\n' > "$root/CMakeLists.txt"

fails=0
expect() {
  local want_status="$1" want_text="$2" tag="$3"
  local out status
  out="$("$here/check-version.sh" "$tag" "$root" 2>&1)"
  status=$?
  if [[ $status -ne $want_status || "$out" != *"$want_text"* ]]; then
    echo "FAIL: tag '$tag' → status $status, output '$out'; want status $want_status containing '$want_text'"
    fails=$((fails + 1))
  fi
}
expect 0 "0.1.2" v0.1.2
expect 0 "0.1.2" test-v0.1.2
expect 0 "0.1.2" test3-v0.1.2
expect 1 "tag v0.1.3 is version 0.1.3 but CMakeLists.txt says 0.1.2" v0.1.3
expect 1 "0.1.2" 0.1.2
expect 1 "CMakeLists.txt says 0.1.2" v0.1.2-rc.1
expect 1 "CMakeLists.txt says 0.1.2" testing-v0.1.2
expect 1 "tag" ""

exact="$("$here/check-version.sh" v0.1.2 "$root")"
if [[ "$exact" != "0.1.2" ]]; then
  echo "FAIL: a matching tag must print only the version, got '$exact'"
  fails=$((fails + 1))
fi

if [[ $fails -ne 0 ]]; then
  exit 1
fi
echo ok
