#!/usr/bin/env bash
# Install Qt 6.8 LTS into the project for local development.
#
# Why: Qt is no longer a vcpkg dependency (building qtbase from source took
# 45-90 minutes per cold cache and exceeded AppVeyor's 60-min job cap). Qt is
# now installed out-of-band via aqtinstall — same tool CI uses, so local and
# CI pick up the exact same Qt version.
#
# Usage:
#   ./tools/setup-qt.sh                    # installs into ~/Qt
#   QT_DIR=/opt/Qt ./tools/setup-qt.sh     # custom install root
#
# After install, export CMAKE_PREFIX_PATH per the printed instructions, then
# run `cmake --preset macos-debug` (or your platform's equivalent).
set -euo pipefail

QT_VERSION="${QT_VERSION:-6.8.0}"
AQT_VERSION="${AQT_VERSION:-3.1.18}"
QT_DIR="${QT_DIR:-$HOME/Qt}"

uname_s="$(uname -s)"
case "$uname_s" in
    Darwin)  qt_platform="mac";    qt_arch="clang_64";   qt_subdir="macos" ;;
    Linux)   qt_platform="linux";  qt_arch="linux_gcc_64"; qt_subdir="gcc_64" ;;
    *)
        echo "Unsupported host: $uname_s. On Windows, run tools/setup-qt.cmd instead." >&2
        exit 1
        ;;
esac

# aqtinstall is a Python tool; install it via pip in a user-local venv to
# avoid clobbering system Python packages.
if ! command -v aqt >/dev/null 2>&1 || \
   ! aqt --version 2>/dev/null | grep -qF "$AQT_VERSION"; then
    echo "Installing aqtinstall ${AQT_VERSION}..."
    if [[ "$uname_s" == "Darwin" ]]; then
        python3 -m pip install --upgrade --quiet --break-system-packages \
            "aqtinstall==${AQT_VERSION}"
    else
        python3 -m pip install --upgrade --quiet --user \
            "aqtinstall==${AQT_VERSION}"
        export PATH="$HOME/.local/bin:$PATH"
    fi
fi

mkdir -p "$QT_DIR"
qt_prefix="${QT_DIR}/${QT_VERSION}/${qt_subdir}"

if [[ -d "$qt_prefix" ]]; then
    echo "Qt ${QT_VERSION} already installed at: $qt_prefix"
else
    echo "Installing Qt ${QT_VERSION} (${qt_platform}/${qt_arch}) into ${QT_DIR}..."
    aqt install-qt "$qt_platform" desktop "$QT_VERSION" "$qt_arch" \
        --outputdir "$QT_DIR" \
        --modules qtdeclarative
fi

cat <<EOF

Qt ${QT_VERSION} is ready at:
    $qt_prefix

To use it with cmake --preset, export CMAKE_PREFIX_PATH:

    export CMAKE_PREFIX_PATH="$qt_prefix"
    cmake --preset macos-debug   # or linux-debug, etc.

To make it persistent, add the export line to ~/.zshrc or ~/.bashrc.

EOF
