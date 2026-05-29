@echo off
REM ChainAPI — AppVeyor Windows configure + build + test.
REM
REM Standalone .cmd so cmd.exe parses the `if` blocks reliably, and so we
REM can activate the MSVC toolchain (vcvars64.bat) before invoking CMake.
REM The Ninja generator needs cl.exe / link.exe / the Windows SDK on PATH,
REM which only vcvars provides — AppVeyor does not auto-load it.
REM
REM Required env vars (persisted earlier via `appveyor SetVariable`):
REM   PRESET, VCPKG_ROOT, VCPKG_BINARY_SOURCES, CMAKE_PREFIX_PATH,
REM   VCPKG_DEFAULT_BINARY_CACHE

setlocal enableextensions

echo === Activate MSVC x64 toolchain ===
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo error: vcvars64.bat not found 1>&2
    exit /b 1
)
call "%VCVARS%"
if errorlevel 1 exit /b 1

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo error: cl.exe not on PATH after vcvars 1>&2
    exit /b 1
)

echo === Configure (%PRESET%) ===
cmake --preset %PRESET%
if errorlevel 1 exit /b 1

echo === Build (%PRESET%) ===
cmake --build --preset %PRESET%
if errorlevel 1 exit /b 1

REM Tests only run on debug presets (release presets don't define a
REM testPreset entry in CMakePresets.json).
if "%PRESET:~-6%" == "-debug" (
    echo === Test (%PRESET%) ===
    ctest --preset %PRESET%
    if errorlevel 1 exit /b 1
) else (
    echo Skipping tests on release preset.
)

echo === Windows build complete ===
endlocal
exit /b 0
