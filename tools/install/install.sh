#!/bin/sh
# Installs the Reqloom CLI:
#
#   curl -fsSL https://reqloom.tech/install.sh | sh
#
# It downloads the archive for this system from the GitHub release, checks its
# SHA-256, unpacks it into ~/.reqloom/<version>/ and links ~/.local/bin/reqloom.
# No sudo, and no shell profile is edited. Run it again to upgrade.
#
#   REQLOOM_VERSION      a tag such as v0.1.2 (default: the latest release)
#   REQLOOM_INSTALL_DIR  where versions are unpacked (default: ~/.reqloom)
#   REQLOOM_BIN_DIR      where the reqloom link goes (default: ~/.local/bin)
#   REQLOOM_BASE_URL     where release files are fetched from
#
# With no REQLOOM_VERSION it installs the release GitHub marks latest, or the
# newest release that has a CLI for this system if that one does not yet.
#
# To uninstall, delete the install directory and the link.
set -eu

REPO_URL="https://github.com/Mirzabaig313/Reqloom"
BUILD_PAGE="https://docs.reqloom.tech/dev/building/"

say() { printf '%s\n' "$*"; }
die() {
  printf 'reqloom install: %s\n' "$*" >&2
  exit 1
}

fetch() { # fetch <url> <file>
  if command -v curl > /dev/null 2>&1; then
    curl -fsSL "$1" -o "$2"
  elif command -v wget > /dev/null 2>&1; then
    wget -q "$1" -O "$2"
  else
    die "curl or wget is required"
  fi
}

sha256_of() {
  if command -v sha256sum > /dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum > /dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    die "sha256sum or shasum is required to check the download"
  fi
}

latest_tag() {
  if [ -n "${REQLOOM_TEST_LATEST:-}" ]; then
    say "$REQLOOM_TEST_LATEST"
    return 0
  fi
  command -v curl > /dev/null 2>&1 || die "curl is required to find the latest release; set REQLOOM_VERSION instead"
  url="$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$REPO_URL/releases/latest")" ||
    die "could not reach $REPO_URL/releases/latest"
  tag="${url##*/}"
  case "$tag" in
    v[0-9]*) say "$tag" ;;
    *) die "could not work out the latest release from $url; set REQLOOM_VERSION" ;;
  esac
}

# The newest published release that has a CLI archive for this system, or
# nothing. Used when the release marked latest does not have one yet: the
# desktop files are attached by a separate pipeline and can arrive first.
newest_tag_with_cli() { # newest_tag_with_cli <platform> <scratch-file>
  api="${REQLOOM_API_URL:-https://api.github.com/repos/Mirzabaig313/Reqloom/releases?per_page=30}"
  fetch "$api" "$2" 2> /dev/null || return 0
  grep -o "releases/download/v[0-9][0-9.]*/reqloom-v[0-9][0-9.]*-$1\.tar\.gz" "$2" 2> /dev/null |
    head -n 1 | sed -E 's#^releases/download/(v[0-9.]+)/.*$#\1#'
}

main() {
  system="${REQLOOM_TEST_UNAME:-$(uname -s) $(uname -m)}"
  case "$system" in
    "Darwin arm64") platform=macos-arm64 ;;
    "Linux x86_64") platform=linux-x86_64 ;;
    *) die "no prebuilt CLI for '$system' (available: macOS on Apple silicon, Linux x86_64). Build from source: $BUILD_PAGE" ;;
  esac

  [ -n "${HOME:-}" ] || [ -n "${REQLOOM_INSTALL_DIR:-}" ] || die "HOME is not set; set REQLOOM_INSTALL_DIR and REQLOOM_BIN_DIR"
  install_dir="${REQLOOM_INSTALL_DIR:-$HOME/.reqloom}"
  bin_dir="${REQLOOM_BIN_DIR:-$HOME/.local/bin}"
  base_url="${REQLOOM_BASE_URL:-$REPO_URL/releases/download}"
  tag="${REQLOOM_VERSION:-}"
  pinned=yes
  if [ -z "$tag" ]; then
    pinned=no
    tag="$(latest_tag)"
  fi

  stage="$(mktemp -d)"
  trap 'rm -rf "$stage"' EXIT INT TERM

  archive="reqloom-$tag-$platform.tar.gz"
  say "Downloading $archive"
  if ! fetch "$base_url/$tag/$archive" "$stage/$archive" 2> /dev/null; then
    other=""
    [ "$pinned" = yes ] || other="$(newest_tag_with_cli "$platform" "$stage/releases.json")"
    if [ -z "$other" ] || [ "$other" = "$tag" ]; then
      die "could not download $base_url/$tag/$archive"
    fi
    say "Release $tag has no CLI for this system yet; installing $other instead."
    tag="$other"
    archive="reqloom-$tag-$platform.tar.gz"
    say "Downloading $archive"
    fetch "$base_url/$tag/$archive" "$stage/$archive" || die "could not download $base_url/$tag/$archive"
  fi
  checksums="reqloom-$tag-checksums.txt"
  fetch "$base_url/$tag/$checksums" "$stage/$checksums" || die "could not download $base_url/$tag/$checksums"

  expected="$(awk -v f="$archive" '$2 == f || $2 == "*" f {print $1}' "$stage/$checksums" | head -n 1)"
  [ -n "$expected" ] || die "checksum for $archive is not listed in $checksums; nothing was installed"
  actual="$(sha256_of "$stage/$archive")"
  [ "$expected" = "$actual" ] ||
    die "checksum mismatch for $archive (expected $expected, got $actual); nothing was installed"

  mkdir "$stage/unpacked"
  tar -xzf "$stage/$archive" -C "$stage/unpacked"
  [ -f "$stage/unpacked/reqloom-$tag/bin/reqloom" ] || die "$archive does not contain reqloom-$tag/bin/reqloom"

  # Everything above happened in the staging folder. The new copy is started
  # once from beside its final place (the staging folder may not allow running
  # programs) before it replaces anything, so a build this system cannot run
  # never becomes the installed one.
  mkdir -p "$install_dir" "$bin_dir"
  incoming="$install_dir/.incoming-$tag"
  rm -rf "$incoming"
  mv "$stage/unpacked/reqloom-$tag" "$incoming"
  if ! "$incoming/bin/reqloom" --version > /dev/null 2>&1; then
    rm -rf "$incoming"
    rmdir "$install_dir" 2> /dev/null || true
    die "reqloom $tag does not start on this system; nothing was installed. The Linux build needs glibc 2.31 or newer (Ubuntu 20.04, Debian 11). Build from source: $BUILD_PAGE"
  fi
  rm -rf "$install_dir/$tag"
  mv "$incoming" "$install_dir/$tag"
  ln -sfn "$install_dir/$tag/bin/reqloom" "$bin_dir/reqloom"

  say "Installed reqloom $tag in $install_dir/$tag"
  say "Linked $bin_dir/reqloom"
  case ":${PATH:-}:" in
    *":$bin_dir:"*) say "Run: reqloom --version" ;;
    *)
      say ""
      say "$bin_dir is not on your PATH. Add this line to your shell profile, then open a new terminal:"
      say "  export PATH=\"$bin_dir:\$PATH\""
      ;;
  esac
}

# Called on the last line, so a download cut short cannot run half a script.
main "$@"
