#!/usr/bin/env bash
#
# tools/build-incremental.sh — used by the cpp-build-on-edit hook.
#
# Defensive incremental build: skips cleanly if the project has never been
# configured, so a fresh checkout doesn't spew errors on every save.
#
# Picks the matching preset for the current OS. Override with $CHAINAPI_PRESET.

set -uo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

# Default preset by OS
case "$(uname -s)" in
  Darwin)  default_preset="macos-debug" ;;
  Linux)   default_preset="linux-debug" ;;
  MINGW*|MSYS*|CYGWIN*) default_preset="windows-debug" ;;
  *)       default_preset="macos-debug" ;;
esac

preset="${CHAINAPI_PRESET:-$default_preset}"
build_dir="build/$preset"

if [[ ! -d "$build_dir" ]]; then
  cat <<EOF >&2
[cpp-build-on-edit] skipping build — preset '$preset' has not been configured.
  Set up the project once with:
    cmake --preset $preset

  After that, this hook will run incremental builds on every save.
EOF
  exit 0
fi

# Trim noisy output: keep the last 80 lines so errors are visible but cold
# rebuilds don't flood the chat.
cmake --build --preset "$preset" 2>&1 | tail -80
status=${PIPESTATUS[0]}

if [[ $status -ne 0 ]]; then
  echo
  echo "[cpp-build-on-edit] build failed (exit $status). Fix or use /cpp-build-resolver." >&2
fi

exit "$status"
