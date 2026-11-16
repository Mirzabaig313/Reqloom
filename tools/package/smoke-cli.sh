#!/bin/sh
# smoke-cli.sh <archive.tar.gz> <repo-root> [expected-version]
# Proves a packaged CLI runs with nothing but its own files: unpacks it into a
# fresh folder whose name has a space, clears the environment so no installed
# Qt can help, then runs --version and lints the bundled sample.
set -eu

archive="$1"
repo="$2"
expected="${3:-}"
# A caller that passes a version must pass a real one: an empty value means its
# own version lookup failed, and must not turn this into the loose check.
if [ "$#" -ge 3 ] && [ -z "$expected" ]; then
  echo "smoke: an empty expected version was passed" >&2
  exit 1
fi

case "$archive" in /*) ;; *) archive="$(pwd)/$archive" ;; esac
case "$repo" in /*) ;; *) repo="$(pwd)/$repo" ;; esac

base="$(mktemp -d)"
trap 'rm -rf "$base"' EXIT
work="$base/smoke test"
mkdir -p "$work"
tar -xzf "$archive" -C "$work"

bin="$(find "$work" -type f -path '*/bin/reqloom' | head -n 1)"
if [ -z "$bin" ]; then
  echo "smoke: no bin/reqloom in $archive" >&2
  exit 1
fi

# macOS records absolute library paths in the binary, so an unbundled library
# on the build machine would load here and be missing for users.
root="$(dirname "$(dirname "$bin")")"
if [ "$(uname -s)" = "Darwin" ]; then
  outside="$(find "$root" -type f \( -perm -u+x -o -name '*.dylib' \) -exec otool -L {} \; 2>/dev/null |
    awk '/^\t/ {print $1}' | grep -Ev '^(/usr/lib/|/System/|@rpath/|@executable_path/|@loader_path/)' | sort -u || true)"
  if [ -n "$outside" ]; then
    echo "smoke: the package loads libraries from outside itself:" >&2
    echo "$outside" >&2
    exit 1
  fi
  # A run path left over from the build machine would satisfy @rpath there and
  # hide a broken bundle; every run path must be relative to the package.
  stray="$(find "$root" -type f \( -perm -u+x -o -name '*.dylib' \) -exec otool -l {} \; 2>/dev/null |
    awk '/LC_RPATH/ {want=1} want && $1 == "path" {print $2; want=0}' |
    grep -Ev '^(@executable_path|@loader_path)' | sort -u || true)"
  if [ -n "$stray" ]; then
    echo "smoke: the package has run paths that point outside itself:" >&2
    echo "$stray" >&2
    exit 1
  fi
elif command -v ldd >/dev/null 2>&1; then
  missing="$(ldd "$bin" 2>/dev/null | grep 'not found' || true)"
  if [ -n "$missing" ]; then
    echo "smoke: libraries not found:" >&2
    echo "$missing" >&2
    exit 1
  fi
fi

run() {
  env -i HOME="$base" PATH=/usr/bin:/bin "$bin" "$@"
}

version="$(run --version)"
echo "$version"
if [ -n "$expected" ]; then
  if [ "$version" != "reqloom $expected" ]; then
    echo "smoke: expected 'reqloom $expected', got '$version'" >&2
    exit 1
  fi
else
  case "$version" in
    "reqloom "*) ;;
    *)
      echo "smoke: unexpected --version output" >&2
      exit 1
      ;;
  esac
fi

lint="$(run lint "$repo/samples/marketplace")"
echo "$lint"
case "$lint" in
  *"LINT OK"*) ;;
  *)
    echo "smoke: lint of the sample did not report LINT OK" >&2
    exit 1
    ;;
esac
echo "smoke: ok"
