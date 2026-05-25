@echo off
REM Install Qt 6.8 LTS into the project for local development on Windows.
REM
REM Why: Qt is no longer a vcpkg dependency (building qtbase from source took
REM 45-90 minutes per cold cache and exceeded AppVeyor's 60-min job cap). Qt
REM is now installed out-of-band via aqtinstall — same tool CI uses, so local
REM and CI pick up the exact same Qt version.
REM
REM Usage:
REM     tools\setup-qt.cmd                  installs into C:\Qt
REM     set QT_DIR=D:\Qt
REM     tools\setup-qt.cmd                  custom install root
REM
REM After install, set CMAKE_PREFIX_PATH per the printed instructions, then
REM run `cmake --preset windows-debug`.
setlocal enabledelayedexpansion

if "%QT_VERSION%"=="" set QT_VERSION=6.8.0
if "%AQT_VERSION%"=="" set AQT_VERSION=3.1.18
if "%QT_DIR%"=="" set QT_DIR=C:\Qt

set QT_PREFIX=%QT_DIR%\%QT_VERSION%\msvc2022_64

REM Install aqtinstall via pip if not already at the pinned version.
aqt --version >nul 2>&1
if errorlevel 1 (
    echo Installing aqtinstall %AQT_VERSION%...
    python -m pip install --upgrade --quiet "aqtinstall==%AQT_VERSION%"
)

if not exist "%QT_DIR%" mkdir "%QT_DIR%"

if exist "%QT_PREFIX%" (
    echo Qt %QT_VERSION% already installed at: %QT_PREFIX%
) else (
    echo Installing Qt %QT_VERSION% (windows/win64_msvc2022_64) into %QT_DIR%...
    aqt install-qt windows desktop %QT_VERSION% win64_msvc2022_64 ^
        --outputdir "%QT_DIR%" ^
        --modules qtdeclarative
)

echo.
echo Qt %QT_VERSION% is ready at:
echo     %QT_PREFIX%
echo.
echo To use it with cmake --preset, set CMAKE_PREFIX_PATH:
echo.
echo     set CMAKE_PREFIX_PATH=%QT_PREFIX%
echo     cmake --preset windows-debug
echo.
echo To make it persistent, set CMAKE_PREFIX_PATH in System Properties.
echo.
endlocal
