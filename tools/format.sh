#!/usr/bin/env bash
# Formats all C++ sources under engine/, cli/, desktop/, and ipc/.
# Honors .clang-format at the repo root.
#
# Pinned to clang-format 18 so local runs match CI exactly. clang-format
# bumps minor versions almost every release, and even a 19→18 difference
# can introduce or hide formatting drift on AWS-style long lines, raw
# string literals, etc.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Pick clang-format in this order:
#   1. $CLANG_FORMAT       — explicit override
#   2. clang-format-18     — the version CI runs (Linux apt name)
#   3. /opt/homebrew/opt/llvm@18/bin/clang-format  — Homebrew keg path
#   4. clang-format        — fallback; may version-skew vs CI
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
    echo "warning: using \$(which clang-format), not pinned to v18 — CI may disagree" >&2
else
    cat >&2 <<'EOF'
error: no clang-format found on PATH.

Install clang-format 18 (matches CI):
  macOS:  brew install llvm@18
          export PATH="/opt/homebrew/opt/llvm@18/bin:$PATH"
  Linux:  sudo apt-get install -y clang-format-18
EOF
    exit 1
fi

actual_version="$("$clang_format" --version | head -n1)"
if ! grep -q 'version 18' <<<"$actual_version"; then
    echo "warning: $clang_format reports '$actual_version' — CI uses v18, drift possible" >&2
fi

find "$ROOT/engine" "$ROOT/cli" "$ROOT/desktop" "$ROOT/ipc" \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' \) \
    -not -path '*/build/*' \
    -print0 \
    | xargs -0 "$clang_format" -i

echo "Formatted with $actual_version."
