#!/bin/sh
# Tests install.sh offline: every case builds a fake release in a temp folder
# and points the script at it with a file:// base URL.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
script="$here/install.sh"

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) platform=macos-arm64 ;;
  Linux-x86_64) platform=linux-x86_64 ;;
  *) platform=linux-x86_64; FORCE_UNAME="Linux x86_64" ;;
esac

sha() {
  if command -v sha256sum > /dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi
}

# make_release <root> <tag> <version>
make_release() {
  dir="$1/releases/$2"
  mkdir -p "$dir" "$1/build/reqloom-$2/bin" "$1/build/reqloom-$2/lib"
  printf '#!/bin/sh\necho "reqloom %s"\n' "$3" > "$1/build/reqloom-$2/bin/reqloom"
  chmod +x "$1/build/reqloom-$2/bin/reqloom"
  tar -czf "$dir/reqloom-cli-$2-$platform.tar.gz" -C "$1/build" "reqloom-$2"
  printf '%s  %s\n' "$(sha "$dir/reqloom-cli-$2-$platform.tar.gz")" "reqloom-cli-$2-$platform.tar.gz" > "$dir/reqloom-cli-$2-checksums.txt"
}

# make_broken_release <root> <tag>: the binary is present but cannot start,
# as on a system whose C library is too old for it.
make_broken_release() {
  make_release "$1" "$2" unused
  printf '#!/bin/sh\necho "version GLIBC_2.31 not found" >&2\nexit 127\n' > "$1/build/reqloom-$2/bin/reqloom"
  tar -czf "$1/releases/$2/reqloom-cli-$2-$platform.tar.gz" -C "$1/build" "reqloom-$2"
  printf '%s  %s\n' "$(sha "$1/releases/$2/reqloom-cli-$2-$platform.tar.gz")" "reqloom-cli-$2-$platform.tar.gz" > "$1/releases/$2/reqloom-cli-$2-checksums.txt"
}

# run_install <root> <tag> [extra env assignments...]; sets $status and $output
run_install() {
  root="$1"
  tag="$2"
  shift 2
  output="$(env -i PATH=/usr/bin:/bin HOME="$root/home" \
    REQLOOM_BASE_URL="file://$root/releases" REQLOOM_VERSION="$tag" \
    REQLOOM_INSTALL_DIR="$root/install" REQLOOM_BIN_DIR="$root/bin" \
    ${FORCE_UNAME:+REQLOOM_TEST_UNAME="$FORCE_UNAME"} "$@" ${TEST_SHELL:-sh} "$script" 2>&1)"
  status=$?
}

passed=0
failed=0
ok() { passed=$((passed + 1)); }
fail() {
  failed=$((failed + 1))
  echo "FAIL: $1"
  echo "  status=$status output: $output" | head -n 6
}
fresh() {
  root="$(mktemp -d)"
  mkdir -p "$root/home"
}

fresh; make_release "$root" v9.9.9 9.9.9
run_install "$root" v9.9.9
if [ $status -eq 0 ] && [ "$("$root/bin/reqloom" --version 2> /dev/null)" = "reqloom 9.9.9" ] &&
  echo "$output" | grep -q "Installed reqloom v9.9.9"; then ok; else fail installs_and_links; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9
run_install "$root" v9.9.9
if [ $status -eq 0 ] && echo "$output" | grep -qF "export PATH=\"$root/bin:\$PATH\"" &&
  [ ! -e "$root/home/.profile" ] && [ ! -e "$root/home/.bashrc" ] && [ ! -e "$root/home/.zshrc" ] &&
  [ -z "$(ls -A "$root/home")" ]; then ok; else fail prints_path_line_and_edits_no_profile; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9
run_install "$root" v9.9.9 PATH="$root/bin:/usr/bin:/bin"
if [ $status -eq 0 ] && ! echo "$output" | grep -q "export PATH"; then ok; else fail no_path_line_when_already_on_path; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9
printf 'x' >> "$root/releases/v9.9.9/reqloom-cli-v9.9.9-$platform.tar.gz"
run_install "$root" v9.9.9
if [ $status -eq 1 ] && echo "$output" | grep -qi "checksum" &&
  [ ! -e "$root/install/v9.9.9" ] && [ ! -e "$root/bin/reqloom" ]; then ok; else fail checksum_mismatch_leaves_nothing; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9
rm "$root/releases/v9.9.9/reqloom-cli-v9.9.9-$platform.tar.gz"
run_install "$root" v9.9.9
if [ $status -eq 1 ] && [ ! -e "$root/install" ] && [ ! -e "$root/bin/reqloom" ]; then ok; else fail missing_archive_fails_cleanly; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9
: > "$root/releases/v9.9.9/reqloom-cli-v9.9.9-checksums.txt"
run_install "$root" v9.9.9
if [ $status -eq 1 ] && echo "$output" | grep -qi "checksum" && [ ! -e "$root/bin/reqloom" ]; then ok; else fail archive_absent_from_checksum_file_is_refused; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9; make_release "$root" v9.9.10 9.9.10
run_install "$root" v9.9.9
run_install "$root" v9.9.10
if [ $status -eq 0 ] && [ "$("$root/bin/reqloom" --version)" = "reqloom 9.9.10" ] && [ -d "$root/install/v9.9.9" ]; then ok; else fail rerun_upgrades; fi
run_install "$root" v9.9.10
if [ $status -eq 0 ] && [ "$("$root/bin/reqloom" --version)" = "reqloom 9.9.10" ]; then ok; else fail reinstalling_the_same_version_works; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9
run_install "$root" v9.9.9 REQLOOM_TEST_UNAME="Darwin x86_64"
if [ $status -eq 1 ] && echo "$output" | grep -qF "https://docs.reqloom.tech/dev/building/" && [ ! -e "$root/install" ]; then ok; else fail unsupported_system_names_the_build_page; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9
mkdir -p "$root/space dir"
run_install "$root" v9.9.9 REQLOOM_INSTALL_DIR="$root/space dir/install" REQLOOM_BIN_DIR="$root/space dir/bin"
if [ $status -eq 0 ] && [ "$("$root/space dir/bin/reqloom" --version)" = "reqloom 9.9.9" ]; then ok; else fail install_paths_with_spaces; fi
rm -rf "$root"

fresh; make_broken_release "$root" v9.9.9
run_install "$root" v9.9.9
if [ $status -eq 1 ] && echo "$output" | grep -q "does not start" && echo "$output" | grep -q "glibc 2.31" &&
  echo "$output" | grep -qF "https://docs.reqloom.tech/dev/building/" &&
  [ ! -e "$root/bin/reqloom" ] && [ -z "$(ls -A "$root/install" 2> /dev/null)" ]; then ok; else fail a_binary_that_cannot_start_is_not_installed; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9; make_broken_release "$root" v9.9.10
run_install "$root" v9.9.9
run_install "$root" v9.9.10
if [ $status -eq 1 ] && [ "$("$root/bin/reqloom" --version)" = "reqloom 9.9.9" ] && [ ! -e "$root/install/v9.9.10" ]; then ok; else fail a_failed_upgrade_keeps_the_working_version; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9; make_broken_release "$root" v9.9.9b
cp "$root/releases/v9.9.9b/reqloom-cli-v9.9.9b-$platform.tar.gz" "$root/broken.tar.gz"
run_install "$root" v9.9.9
( cd "$root/build" && rm -rf reqloom-v9.9.9 && mkdir -p reqloom-v9.9.9/bin &&
  printf '#!/bin/sh\nexit 127\n' > reqloom-v9.9.9/bin/reqloom && chmod +x reqloom-v9.9.9/bin/reqloom &&
  tar -czf "$root/releases/v9.9.9/reqloom-cli-v9.9.9-$platform.tar.gz" reqloom-v9.9.9 )
printf '%s  %s\n' "$(sha "$root/releases/v9.9.9/reqloom-cli-v9.9.9-$platform.tar.gz")" "reqloom-cli-v9.9.9-$platform.tar.gz" > "$root/releases/v9.9.9/reqloom-cli-v9.9.9-checksums.txt"
run_install "$root" v9.9.9
if [ $status -eq 1 ] && [ "$("$root/bin/reqloom" --version)" = "reqloom 9.9.9" ]; then ok; else fail a_failed_reinstall_of_the_same_version_keeps_it; fi
rm -rf "$root"

# The release GitHub marks latest may have no CLI archive yet (the desktop files
# are published by another pipeline); the newest release that has one is used.
fresh; make_release "$root" v9.9.9 9.9.9; mkdir -p "$root/releases/v9.9.10"
printf '[{"tag_name":"v9.9.10","assets":[]},{"tag_name":"v9.9.9","assets":[{"browser_download_url":"https://github.com/Mirzabaig313/Reqloom/releases/download/v9.9.9/reqloom-cli-v9.9.9-%s.tar.gz"}]}]' "$platform" > "$root/api.json"
run_install "$root" "" REQLOOM_TEST_LATEST=v9.9.10 REQLOOM_API_URL="file://$root/api.json"
if [ $status -eq 0 ] && [ "$("$root/bin/reqloom" --version)" = "reqloom 9.9.9" ] && echo "$output" | grep -q "v9.9.10"; then ok; else fail latest_without_a_cli_falls_back_to_the_newest_with_one; fi
rm -rf "$root"

fresh; make_release "$root" v9.9.9 9.9.9; mkdir -p "$root/releases/v9.9.10"
printf '[]' > "$root/api.json"
run_install "$root" v9.9.10 REQLOOM_API_URL="file://$root/api.json"
if [ $status -eq 1 ] && [ ! -e "$root/bin/reqloom" ]; then ok; else fail a_named_version_is_never_replaced_by_another; fi
rm -rf "$root"

echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
