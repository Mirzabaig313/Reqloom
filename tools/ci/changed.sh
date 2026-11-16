#!/usr/bin/env bash
# changed.sh <before-sha> <after-sha> <path>...
# Prints "true" when anything under the paths differs between the two commits,
# and whenever that cannot be known (no usable <before-sha>), so a doubt never
# skips work. Prints "false" otherwise.
set -uo pipefail

before="${1:-}"
after="${2:-}"
shift 2 2>/dev/null || true

if [[ -z "$before" || "$before" =~ ^0+$ ]] ||
  ! git cat-file -e "${before}^{commit}" 2>/dev/null ||
  ! git cat-file -e "${after}^{commit}" 2>/dev/null; then
  echo true
  exit 0
fi

if git diff --quiet "$before" "$after" -- "$@"; then
  echo false
else
  echo true
fi
