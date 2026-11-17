# Tests install.ps1 offline: each case builds a fake release in a temp folder
# and points the script at it with REQLOOM_BASE_URL. REQLOOM_NO_PATH=1 keeps the
# machine's PATH untouched except in the one case that tests it, which restores it.
$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot 'install.ps1'
$passed = 0
$failed = 0

# The stand-in for reqloom.exe is Windows' own tar.exe, which starts and answers
# --version with exit code 0. -Broken writes a text file instead: a program
# Windows cannot start, as when a required DLL is missing.
function New-FakeRelease([string]$Root, [string]$Tag, [switch]$Broken) {
    $dir = Join-Path $Root "releases\$Tag"
    $payload = Join-Path $Root "build\reqloom-$Tag"
    New-Item -ItemType Directory -Force -Path $dir, $payload | Out-Null
    if ($Broken) {
        Set-Content -LiteralPath (Join-Path $payload 'reqloom.exe') -Value "not a program"
    } else {
        Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\tar.exe') -Destination (Join-Path $payload 'reqloom.exe')
    }
    $zip = Join-Path $dir "reqloom-cli-$Tag-windows-x64.zip"
    Compress-Archive -Path $payload -DestinationPath $zip -Force
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath (Join-Path $dir "reqloom-cli-$Tag-checksums.txt") -Value "$hash  reqloom-cli-$Tag-windows-x64.zip"
}

# Returns @{ Ok = <bool>; Output = <string> }
function Invoke-Install([string]$Root, [string]$Tag, [hashtable]$Extra = @{}) {
    $saved = @{}
    $vars = @{
        REQLOOM_BASE_URL    = (Join-Path $Root 'releases')
        REQLOOM_VERSION     = $Tag
        REQLOOM_INSTALL_DIR = (Join-Path $Root 'install')
        REQLOOM_NO_PATH     = '1'
        REQLOOM_TEST_ARCH   = 'AMD64'
    }
    foreach ($key in $Extra.Keys) { $vars[$key] = $Extra[$key] }
    foreach ($key in $vars.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key)
        [Environment]::SetEnvironmentVariable($key, $vars[$key])
    }
    try {
        $output = (& $installer *>&1 | Out-String)
        return @{ Ok = $true; Output = $output }
    } catch {
        return @{ Ok = $false; Output = "$_" }
    } finally {
        foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key]) }
    }
}

function Assert-Case([string]$Name, [bool]$Condition, $Result) {
    if ($Condition) { $script:passed++ } else {
        $script:failed++
        Write-Host "FAIL: $Name"
        Write-Host "  ok=$($Result.Ok) output: $($Result.Output)"
    }
}

function New-Root { $root = Join-Path ([IO.Path]::GetTempPath()) ("reqloom test " + [Guid]::NewGuid().ToString('N')); New-Item -ItemType Directory -Force -Path $root | Out-Null; $root }

$root = New-Root; New-FakeRelease $root 'v9.9.9'
$r = Invoke-Install $root 'v9.9.9'
$shim = Join-Path $root 'install\bin\reqloom.cmd'
Assert-Case 'installs_and_writes_the_launcher' ($r.Ok -and
    (Test-Path (Join-Path $root 'install\v9.9.9\reqloom.exe')) -and
    (Test-Path $shim) -and ((Get-Content $shim -Raw).Trim() -eq '@"%~dp0..\v9.9.9\reqloom.exe" %*') -and
    ($r.Output -match 'Installed reqloom v9\.9\.9')) $r
Remove-Item $root -Recurse -Force

$root = New-Root; New-FakeRelease $root 'v9.9.9'
Add-Content -LiteralPath (Join-Path $root 'releases\v9.9.9\reqloom-cli-v9.9.9-windows-x64.zip') -Value 'x'
$r = Invoke-Install $root 'v9.9.9'
Assert-Case 'checksum_mismatch_leaves_nothing' ((-not $r.Ok) -and ($r.Output -match 'checksum') -and
    (-not (Test-Path (Join-Path $root 'install')))) $r
Remove-Item $root -Recurse -Force

$root = New-Root; New-FakeRelease $root 'v9.9.9'
Remove-Item (Join-Path $root 'releases\v9.9.9\reqloom-cli-v9.9.9-windows-x64.zip')
$r = Invoke-Install $root 'v9.9.9'
Assert-Case 'missing_archive_fails_cleanly' ((-not $r.Ok) -and (-not (Test-Path (Join-Path $root 'install')))) $r
Remove-Item $root -Recurse -Force

$root = New-Root; New-FakeRelease $root 'v9.9.9'; New-FakeRelease $root 'v9.9.10'
$null = Invoke-Install $root 'v9.9.9'
$r = Invoke-Install $root 'v9.9.10'
Assert-Case 'rerun_upgrades' ($r.Ok -and
    ((Get-Content (Join-Path $root 'install\bin\reqloom.cmd') -Raw) -match 'v9\.9\.10') -and
    (Test-Path (Join-Path $root 'install\v9.9.9'))) $r
Remove-Item $root -Recurse -Force

$root = New-Root; New-FakeRelease $root 'v9.9.9'
$r = Invoke-Install $root 'v9.9.9' @{ REQLOOM_TEST_ARCH = 'ARM64' }
Assert-Case 'unsupported_cpu_names_the_build_page' ((-not $r.Ok) -and
    ($r.Output -match [regex]::Escape('https://docs.reqloom.tech/dev/building/')) -and
    (-not (Test-Path (Join-Path $root 'install')))) $r
Remove-Item $root -Recurse -Force

# Saves and restores the user PATH exactly as stored (unexpanded, same registry type).
function Get-RawUserPath {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    $had = $key.GetValueNames() -contains 'Path'
    $state = @{
        Had  = $had
        Raw  = [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        Kind = if ($had) { $key.GetValueKind('Path') } else { [Microsoft.Win32.RegistryValueKind]::ExpandString }
    }
    $key.Close()
    $state
}
function Restore-RawUserPath($State) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    if ($State.Had) { $key.SetValue('Path', $State.Raw, $State.Kind) } else { $key.DeleteValue('Path', $false) }
    $key.Close()
}

$root = New-Root; New-FakeRelease $root 'v9.9.9'
$saved = Get-RawUserPath
try {
    $r = Invoke-Install $root 'v9.9.9'
    Assert-Case 'no_path_flag_leaves_path_alone' ($r.Ok -and ((Get-RawUserPath).Raw -eq $saved.Raw)) $r
    $r = Invoke-Install $root 'v9.9.9' @{ REQLOOM_NO_PATH = '0' }
    $bin = Join-Path $root 'install\bin'
    Assert-Case 'adds_the_bin_folder_to_the_user_path_once' ($r.Ok -and ((Get-RawUserPath).Raw.Split(';') -contains $bin)) $r
    $r = Invoke-Install $root 'v9.9.9' @{ REQLOOM_NO_PATH = '0' }
    Assert-Case 'does_not_add_it_twice' ($r.Ok -and
        (@((Get-RawUserPath).Raw.Split(';') | Where-Object { $_ -eq $bin }).Count -eq 1)) $r
} finally {
    Restore-RawUserPath $saved
}
Remove-Item $root -Recurse -Force

$root = New-Root; New-FakeRelease $root 'v9.9.9' -Broken
$r = Invoke-Install $root 'v9.9.9'
Assert-Case 'a_binary_that_cannot_start_is_not_installed' ((-not $r.Ok) -and ($r.Output -match 'does not start') -and
    (-not (Test-Path (Join-Path $root 'install\v9.9.9'))) -and
    (-not (Test-Path (Join-Path $root 'install\bin\reqloom.cmd')))) $r
Remove-Item $root -Recurse -Force

$root = New-Root; New-FakeRelease $root 'v9.9.9'; New-FakeRelease $root 'v9.9.10' -Broken
$null = Invoke-Install $root 'v9.9.9'
$r = Invoke-Install $root 'v9.9.10'
Assert-Case 'a_failed_upgrade_keeps_the_working_version' ((-not $r.Ok) -and
    ((Get-Content (Join-Path $root 'install\bin\reqloom.cmd') -Raw) -match 'v9\.9\.9\\') -and
    (Test-Path (Join-Path $root 'install\v9.9.9\reqloom.exe')) -and
    (-not (Test-Path (Join-Path $root 'install\v9.9.10')))) $r
Remove-Item $root -Recurse -Force

# An entry written with a variable must stay written that way: expanding it
# would freeze the user's PATH to today's folder names.
$root = New-Root; New-FakeRelease $root 'v9.9.9'
$saved = Get-RawUserPath
try {
    $marker = '%USERPROFILE%\reqloom-test-marker'
    $seed = if ($saved.Raw) { "$($saved.Raw);$marker" } else { $marker }
    Restore-RawUserPath @{ Had = $true; Raw = $seed; Kind = [Microsoft.Win32.RegistryValueKind]::ExpandString }
    $r = Invoke-Install $root 'v9.9.9' @{ REQLOOM_NO_PATH = '0' }
    $after = Get-RawUserPath
    Assert-Case 'path_entries_written_with_variables_stay_unexpanded' ($r.Ok -and
        ($after.Raw.Split(';') -contains $marker) -and
        ($after.Raw.Split(';') -contains (Join-Path $root 'install\bin')) -and
        ($after.Kind -eq [Microsoft.Win32.RegistryValueKind]::ExpandString)) $r
} finally {
    Restore-RawUserPath $saved
}
Remove-Item $root -Recurse -Force

Write-Host "$passed passed, $failed failed"
if ($failed -ne 0) { exit 1 }
