# package-cli.ps1 -BuildDir <dir> -Tag <tag> -OutDir <dir> [-ExpectedVersion <x.y.z>]
# Writes reqloom-<tag>-windows-x64.zip holding reqloom-<tag>\reqloom.exe with the
# Qt and vcpkg DLLs beside it, then runs the packaged binary from a clean folder.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$ExpectedVersion = ""
)
$ErrorActionPreference = "Stop"

# A caller that passes a version must pass a real one: an empty value means its
# own version lookup failed, and must not skip the comparison below.
if ($PSBoundParameters.ContainsKey('ExpectedVersion') -and -not $ExpectedVersion) {
    throw "package: an empty expected version was passed"
}

$exe = Join-Path $BuildDir "cli/reqloom.exe"
if (-not (Test-Path $exe)) { throw "package: $exe not found" }
if (-not $env:QT_ROOT_DIR) { throw "package: QT_ROOT_DIR is not set" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stage = Join-Path ([IO.Path]::GetTempPath()) ("reqloom pkg " + [Guid]::NewGuid().ToString("N"))
$name = "reqloom-$Tag"
$dir = Join-Path $stage $name
New-Item -ItemType Directory -Force -Path $dir | Out-Null
try {
    Copy-Item $exe $dir
    & "$env:QT_ROOT_DIR/bin/windeployqt.exe" --release --no-translations --no-compiler-runtime (Join-Path $dir "reqloom.exe")
    if ($LASTEXITCODE -ne 0) { throw "package: windeployqt failed" }

    # windeployqt copies only Qt's DLLs; vcpkg's are dynamic on this triplet.
    $vbin = Get-ChildItem -Path (Join-Path $BuildDir "vcpkg_installed") -Directory |
        Where-Object { $_.Name -ne 'vcpkg' } |
        ForEach-Object { Join-Path $_.FullName 'bin' } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if (-not $vbin) { throw "package: vcpkg bin directory not found under $BuildDir/vcpkg_installed" }
    Copy-Item "$vbin/*.dll" $dir -Force

    # The C++ runtime is not part of Windows; a clean machine or container has
    # none, or an older one. The runner has it in System32, which would hide that.
    if (-not $env:VCToolsRedistDir) { throw "package: VCToolsRedistDir is not set (MSVC environment missing)" }
    $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
        Select-Object -First 1
    if (-not $crt) { throw "package: MSVC runtime not found under $env:VCToolsRedistDir" }
    Copy-Item (Join-Path $crt.FullName '*.dll') $dir -Force
    foreach ($needed in @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')) {
        if (-not (Test-Path (Join-Path $dir $needed))) { throw "package: $needed was not copied" }
    }

    # Run it with a PATH that holds only Windows itself, so a DLL that was not
    # copied fails here instead of on a user's machine.
    $savedPath = $env:PATH
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    try {
        $version = (& (Join-Path $dir "reqloom.exe") --version | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { throw "package: the packaged reqloom.exe did not start" }
    } finally {
        $env:PATH = $savedPath
    }
    Write-Host $version
    if ($ExpectedVersion -and $version -ne "reqloom $ExpectedVersion") {
        throw "package: expected 'reqloom $ExpectedVersion', got '$version'"
    }

    $archive = Join-Path (Resolve-Path $OutDir) "$name-windows-x64.zip"
    Compress-Archive -Path $dir -DestinationPath $archive -Force
    Write-Host $archive
} finally {
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
}
