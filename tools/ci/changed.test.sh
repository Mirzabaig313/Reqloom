#!/usr/bin/env bash
# Tests tools/ci/changed.sh against a throwaway repository.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(mktemp -d)"
trap 'rm -rf "$repo"' EXIT
git -C "$repo" init -q
git -C "$repo" config user.email t@example.invalid
git -C "$repo" config user.name t
mkdir -p "$repo/engine" "$repo/cli"
echo 1 > "$repo/engine/a"
git -C "$repo" add -A
git -C "$repo" commit -qm one
c1="$(git -C "$repo" rev-parse HEAD)"
echo 1 > "$repo/cli/b"
git -C "$repo" add -A
git -C "$repo" commit -qm two
c2="$(git -C "$repo" rev-parse HEAD)"

fails=0
check() {
  local want="$1"
  shift
  local got
  got="$(cd "$repo" && "$here/changed.sh" "$@")"
  if [[ "$got" != "$want" ]]; then
    echo "FAIL: changed.sh $* → '$got', want '$want'"
    fails=$((fails + 1))
  fi
}
check false "$c1" "$c2" engine
check true "$c1" "$c2" cli
check true "$c1" "$c2" engine cli
check true "" "$c2" engine
check true 0000000000000000000000000000000000000000 "$c2" engine
check true deadbeef "$c2" engine
if [[ $fails -ne 0 ]]; then
  exit 1
fi
echo ok
