#!/usr/bin/env bash
# package-cli.sh <build-dir> <tag> <out-dir>
# Writes reqloom-<tag>-macos-arm64.tar.gz or reqloom-<tag>-linux-x86_64.tar.gz:
#   reqloom-<tag>/bin/reqloom
#   reqloom-<tag>/lib/…        every library the binary needs that the system
#                              itself does not provide, found relative to bin/
set -euo pipefail

build="$1"
tag="$2"
out="$3"

binary="$build/cli/reqloom"
[[ -x "$binary" ]] || { echo "package: $binary not found" >&2; exit 1; }
mkdir -p "$out"
out="$(cd "$out" && pwd)"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
name="reqloom-$tag"
mkdir -p "$stage/$name/bin" "$stage/$name/lib"
cp "$binary" "$stage/$name/bin/reqloom"
chmod u+w "$stage/$name/bin/reqloom"

package_macos() {
  local lib="$stage/$name/lib"
  local map="$stage/map.tsv" # <path under lib/> TAB <original file>
  : > "$map"

  # Framework binaries keep their X.framework/Versions/A/X path under lib/;
  # plain dylibs sit directly in lib/. Either is referenced as @rpath/<that>.
  bundled_path() {
    case "$1" in
      *.framework/*) echo "$1" | sed -E 's#^.*/([^/]+\.framework/.*)$#\1#' ;;
      *) basename "$1" ;;
    esac
  }
  # Prints the file a dependency refers to, or nothing for a system library.
  resolve() {
    local dep="$1" owner_dir="$2" candidate=""
    case "$dep" in
      /usr/lib/* | /System/* | @executable_path/*) return 0 ;;
      @rpath/*)
        for candidate in "$owner_dir/${dep#@rpath/}" "$owner_dir/../${dep#@rpath/}" \
          "$owner_dir/../../../${dep#@rpath/}" "${QT_ROOT_DIR:-/nonexistent}/lib/${dep#@rpath/}"; do
          [[ -f "$candidate" ]] && break
          candidate=""
        done
        ;;
      @loader_path/*) candidate="$owner_dir/${dep#@loader_path/}" ;;
      *) candidate="$dep" ;;
    esac
    [[ -n "$candidate" && -f "$candidate" ]] || { echo "package: cannot find $dep (needed from $owner_dir)" >&2; exit 1; }
    echo "$(cd "$(dirname "$candidate")" && pwd -P)/$(basename "$candidate")"
  }
  dependencies() {
    otool -L "$1" | awk 'NR > 1 {print $1}'
  }

  # First pass: copy the binary's dependencies, and theirs, into lib/.
  local todo="$stage/todo.txt" done_count=0 item dep source rel target
  echo "$binary" > "$todo"
  while [[ "$done_count" -lt "$(wc -l < "$todo")" ]]; do
    done_count=$((done_count + 1))
    item="$(sed -n "${done_count}p" "$todo")"
    while IFS= read -r dep; do
      source="$(resolve "$dep" "$(dirname "$item")")"
      [[ -n "$source" && "$source" != "$item" ]] || continue
      rel="$(bundled_path "$source")"
      grep -q "^$rel	" "$map" && continue
      printf '%s\t%s\n' "$rel" "$source" >> "$map"
      target="$lib/$rel"
      mkdir -p "$(dirname "$target")"
      cp -L "$source" "$target"
      chmod u+w "$target"
      # Qt from the installer is a universal binary; ship only this machine's half.
      if lipo -info "$target" | grep -q 'Architectures in the fat file'; then
        lipo -thin "$(uname -m)" "$target" -output "$target.thin"
        mv "$target.thin" "$target"
      fi
      echo "$source" >> "$todo"
    done < <(dependencies "$item")
  done

  # Second pass: point every reference at the bundled copy.
  rewrite() {
    local file="$1" original="$2" dep source
    while IFS= read -r dep; do
      source="$(resolve "$dep" "$(dirname "$original")")"
      [[ -n "$source" && "$source" != "$original" ]] || continue
      install_name_tool -change "$dep" "@rpath/$(bundled_path "$source")" "$file" 2> /dev/null
    done < <(dependencies "$original")
  }
  # Run paths from the build machine would resolve @rpath there and hide a
  # broken bundle, so none survive.
  drop_rpaths() {
    local file="$1" path
    while IFS= read -r path; do
      install_name_tool -delete_rpath "$path" "$file" 2> /dev/null
    done < <(otool -l "$file" | awk '/LC_RPATH/ {want=1} want && $1 == "path" {print $2; want=0}')
  }
  rewrite "$stage/$name/bin/reqloom" "$binary"
  drop_rpaths "$stage/$name/bin/reqloom"
  install_name_tool -add_rpath @executable_path/../lib "$stage/$name/bin/reqloom" 2> /dev/null

  while IFS=$'\t' read -r rel source; do
    install_name_tool -id "@rpath/$rel" "$lib/$rel" 2> /dev/null
    rewrite "$lib/$rel" "$source"
    drop_rpaths "$lib/$rel"
    codesign --force --sign - "$lib/$rel" 2> /dev/null
  done < "$map"
  codesign --force --sign - "$stage/$name/bin/reqloom" 2> /dev/null
  rm -f "$map" "$todo"
}

package_linux() {
  local lib="$stage/$name/lib" line soname path
  # Everything except glibc itself travels with the binary, so a bare container
  # with no Qt, ICU, libsecret or newer libstdc++ can still start it.
  while IFS= read -r line; do
    soname="$(echo "$line" | awk '{print $1}')"
    path="$(echo "$line" | awk '{print $3}')"
    [[ -n "$path" && -f "$path" ]] || continue
    case "$soname" in
      libc.so.* | libm.so.* | libdl.so.* | libpthread.so.* | librt.so.* | libresolv.so.* | \
        libutil.so.* | libnss_*.so.* | ld-linux*.so.* | linux-vdso.so.*) continue ;;
    esac
    cp -L "$path" "$lib/$soname"
    chmod u+w "$lib/$soname"
    patchelf --set-rpath '$ORIGIN' "$lib/$soname"
  done < <(LD_LIBRARY_PATH="${QT_ROOT_DIR:-}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$binary")
  if LD_LIBRARY_PATH="${QT_ROOT_DIR:-}/lib" ldd "$binary" | grep -q 'not found'; then
    echo "package: unresolved libraries:" >&2
    LD_LIBRARY_PATH="${QT_ROOT_DIR:-}/lib" ldd "$binary" | grep 'not found' >&2
    exit 1
  fi
  patchelf --set-rpath '$ORIGIN/../lib' "$stage/$name/bin/reqloom"
}

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)
    platform=macos-arm64
    package_macos
    ;;
  Linux-x86_64)
    platform=linux-x86_64
    package_linux
    ;;
  *)
    echo "package: unsupported system $(uname -s) $(uname -m)" >&2
    exit 1
    ;;
esac

archive="$out/reqloom-cli-$tag-$platform.tar.gz"
tar -czf "$archive" -C "$stage" "$name"
echo "$archive"
