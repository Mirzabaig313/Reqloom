#!/usr/bin/env bash
#
# tools/pre-push-check.sh
#
# Local smoke check before pushing. Designed for the macOS / Homebrew-Qt
# developer workflow where CI is the authoritative full-vcpkg build.
#
# Steps (in order — each must pass before the next runs):
#   1. clang-format diff check (no formatting drift)
#   2. cmake configure (catches CMakeLists / preset errors)
#   3. cmake build (catches compile + link errors)
#   4. ctest (catches test regressions)
#   5. boundary-check.sh (architectural firewall)
#
# Skip steps with environment variables when iterating:
#   SKIP_FORMAT=1      skip the clang-format dry-run (CI still enforces)
#   SKIP_CONFIGURE=1   skip the cmake --preset step (assume already configured)
#   SKIP_TESTS=1       skip ctest
#   SKIP_BOUNDARY=1    skip the boundary-check grep
#
# Override the preset:
#   CHAINAPI_PRESET=macos-release ./tools/pre-push-check.sh

set -uo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

case "$(uname -s)" in
  Darwin)  default_preset="macos-debug" ;;
  Linux)   default_preset="linux-debug" ;;
  MINGW*|MSYS*|CYGWIN*) default_preset="windows-debug" ;;
  *)       default_preset="macos-debug" ;;
esac

preset="${CHAINAPI_PRESET:-$default_preset}"
build_dir="build/$preset"

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
bold()   { printf '\033[1m%s\033[0m\n' "$*"; }

step() {
  echo
  bold "── $* ──"
}

fail() {
  red "✗ $*"
  exit 1
}

ok() {
  green "✓ $*"
}

# 1. clang-format check — runs the same dry-run --Werror that CI runs, over
#    every C++ source under engine/, cli/, desktop/, ipc/. This mirrors the
#    static-checks job in .github/workflows/build.yml / azure-pipelines.yml.
#    If CI would
#    reject the push for formatting drift, this catches it locally.
step "1/5  clang-format check"

# Pin to clang-format-18 in the same order tools/format.sh does, so local
# and CI never use different versions (which silently introduces drift).
clang_format=""
if [[ -n "${CLANG_FORMAT:-}" ]] && command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  clang_format="$CLANG_FORMAT"
elif command -v clang-format-18 >/dev/null 2>&1; then
  clang_format="clang-format-18"
elif [[ -x /opt/homebrew/opt/llvm@18/bin/clang-format ]]; then
  clang_format="/opt/homebrew/opt/llvm@18/bin/clang-format"
elif [[ -x /usr/local/opt/llvm@18/bin/clang-format ]]; then
  clang_format="/usr/local/opt/llvm@18/bin/clang-format"
elif command -v clang-format >/dev/null 2>&1; then
  clang_format="clang-format"
  yellow "  using \$(which clang-format), not pinned to v18 — CI may disagree"
fi

if [[ -z "$clang_format" ]]; then
  fail "clang-format not on PATH. Install clang-format 18 (brew install llvm@18 on macOS, apt install clang-format-18 on Ubuntu) and ensure it's reachable. Override with SKIP_FORMAT=1 if you must (CI will still reject)."
fi

if [[ "${SKIP_FORMAT:-0}" == "1" ]]; then
  yellow "  SKIP_FORMAT=1 — skipped (CI will still enforce formatting)"
else
  # Portable file collection (no `mapfile` — bash 3.2 on macOS lacks it).
  cpp_files=()
  while IFS= read -r line; do
    cpp_files+=("$line")
  done < <(
    find engine cli desktop ipc \
      \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) \
      -not -path '*/build/*' \
      2>/dev/null
  )

  if [[ ${#cpp_files[@]} -eq 0 ]]; then
    ok "no C++ files to check"
  else
    # Don't pipe through `head` — that swallows clang-format's non-zero exit.
    # Instead capture output, print first 50 lines, then fail on non-zero.
    if format_output="$("$clang_format" --dry-run --Werror "${cpp_files[@]}" 2>&1)"; then
      ok "no clang-format drift across ${#cpp_files[@]} files (using $("$clang_format" --version | head -n1))"
    else
      printf '%s\n' "$format_output" | head -50
      fail "clang-format drift detected. Run tools/format.sh, commit, then re-push."
    fi
  fi
fi

# 2. CMake configure (skipped if SKIP_CONFIGURE=1)
step "2/5  cmake configure"
if [[ "${SKIP_CONFIGURE:-0}" == "1" ]]; then
  yellow "  SKIP_CONFIGURE=1 — skipped"
elif [[ -d "$build_dir" ]]; then
  ok "build dir exists, will reuse on build step"
else
  cmake --preset "$preset" || fail "cmake configure failed"
  ok "configured"
fi

# 3. CMake build
step "3/5  cmake build"
cmake --build --preset "$preset" 2>&1 | tail -50
build_status=${PIPESTATUS[0]}
[[ $build_status -eq 0 ]] || fail "build failed (exit $build_status)"
ok "build green"

# 4. Tests
step "4/5  ctest"
if [[ "${SKIP_TESTS:-0}" == "1" ]]; then
  yellow "  SKIP_TESTS=1 — skipped"
else
  ctest --test-dir "$build_dir" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)" --output-on-failure 2>&1 | tail -30
  test_status=${PIPESTATUS[0]}
  [[ $test_status -eq 0 ]] || fail "tests failed (exit $test_status)"
  ok "all tests passed"
fi

# 5. Boundary check
step "5/5  architectural boundary check"
if [[ "${SKIP_BOUNDARY:-0}" == "1" ]]; then
  yellow "  SKIP_BOUNDARY=1 — skipped"
elif [[ -x tools/ci/boundary-check.sh ]]; then
  ./tools/ci/boundary-check.sh || fail "boundary check failed"
  ok "boundary intact"
else
  yellow "  tools/ci/boundary-check.sh missing — skipped"
fi

echo
green "✓ pre-push checks passed for preset '$preset' — safe to push"
