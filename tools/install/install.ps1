# Installs the Reqloom CLI on Windows:
#
#   irm https://reqloom.tech/install.ps1 | iex
#
# It downloads the archive from the GitHub release, checks its SHA-256, unpacks
# it into %LOCALAPPDATA%\Reqloom\<version>\ and writes a reqloom.cmd launcher in
# %LOCALAPPDATA%\Reqloom\bin, which it adds to your user PATH. No administrator
# rights. Run it again to upgrade.
#
#   REQLOOM_VERSION      a tag such as v0.1.2 (default: the latest release)
#   REQLOOM_INSTALL_DIR  where versions are unpacked (default: %LOCALAPPDATA%\Reqloom)
#   REQLOOM_BASE_URL     where release files are fetched from (a URL or a folder)
#   REQLOOM_NO_PATH      set to 1 to leave PATH alone
#
# To uninstall, delete the install directory and remove its bin folder from PATH.

function Install-Reqloom {
    $ErrorActionPreference = 'Stop'
    # Windows PowerShell 5.1: the progress bar makes downloads many times slower,
    # and older systems do not offer TLS 1.2 to GitHub unless asked.
    $ProgressPreference = 'SilentlyContinue'
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $repoUrl = 'https://github.com/Mirzabaig313/Reqloom'
    $buildPage = 'https://docs.reqloom.tech/dev/building/'

    $arch = if ($env:REQLOOM_TEST_ARCH) { $env:REQLOOM_TEST_ARCH } else { $env:PROCESSOR_ARCHITECTURE }
    if ($arch -ne 'AMD64') {
        throw "reqloom install: no prebuilt CLI for Windows on '$arch' (available: x64). Build from source: $buildPage"
    }

    $installDir = if ($env:REQLOOM_INSTALL_DIR) { $env:REQLOOM_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'Reqloom' }
    $baseUrl = if ($env:REQLOOM_BASE_URL) { $env:REQLOOM_BASE_URL } else { "$repoUrl/releases/download" }

    $tag = $env:REQLOOM_VERSION
    if (-not $tag) {
        $response = Invoke-WebRequest -Uri "$repoUrl/releases/latest" -Method Head -UseBasicParsing
        $final = if ($response.BaseResponse.ResponseUri) { $response.BaseResponse.ResponseUri } else { $response.BaseResponse.RequestMessage.RequestUri }
        $tag = ([string]$final).TrimEnd('/').Split('/')[-1]
        if ($tag -notmatch '^v\d') {
            throw "reqloom install: could not work out the latest release from $final; set REQLOOM_VERSION"
        }
    }

    $archive = "reqloom-$tag-windows-x64.zip"
    $checksums = "reqloom-$tag-checksums.txt"
    $stage = Join-Path ([IO.Path]::GetTempPath()) ("reqloom-install-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    try {
        Write-Host "Downloading $archive"
        foreach ($file in @($archive, $checksums)) {
            $target = Join-Path $stage $file
            if (Test-Path -LiteralPath $baseUrl -PathType Container) {
                $source = Join-Path (Join-Path $baseUrl $tag) $file
                if (-not (Test-Path -LiteralPath $source)) { throw "reqloom install: could not find $source" }
                Copy-Item -LiteralPath $source -Destination $target
            } else {
                try {
                    Invoke-WebRequest -Uri "$baseUrl/$tag/$file" -OutFile $target -UseBasicParsing
                } catch {
                    throw "reqloom install: could not download $baseUrl/$tag/$file"
                }
            }
        }

        $expected = $null
        foreach ($line in Get-Content -LiteralPath (Join-Path $stage $checksums)) {
            $parts = $line -split '\s+', 2
            if ($parts.Count -eq 2 -and $parts[1].TrimStart('*').Trim() -eq $archive) { $expected = $parts[0].ToLowerInvariant() }
        }
        if (-not $expected) {
            throw "reqloom install: checksum for $archive is not listed in $checksums; nothing was installed"
        }
        $actual = (Get-FileHash -LiteralPath (Join-Path $stage $archive) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($expected -ne $actual) {
            throw "reqloom install: checksum mismatch for $archive (expected $expected, got $actual); nothing was installed"
        }

        $unpacked = Join-Path $stage 'unpacked'
        Expand-Archive -LiteralPath (Join-Path $stage $archive) -DestinationPath $unpacked
        $payload = Join-Path $unpacked "reqloom-$tag"
        if (-not (Test-Path -LiteralPath (Join-Path $payload 'reqloom.exe'))) {
            throw "reqloom install: $archive does not contain reqloom-$tag\reqloom.exe"
        }

        # Everything above happened in the staging folder. The new copy is started
        # once from beside its final place before it replaces anything, so a build
        # this system cannot run never becomes the installed one.
        $versionDir = Join-Path $installDir $tag
        $binDir = Join-Path $installDir 'bin'
        $createdInstallDir = -not (Test-Path -LiteralPath $installDir)
        New-Item -ItemType Directory -Force -Path $installDir | Out-Null
        $incoming = Join-Path $installDir ".incoming-$tag"
        if (Test-Path -LiteralPath $incoming) { Remove-Item -LiteralPath $incoming -Recurse -Force }
        Move-Item -LiteralPath $payload -Destination $incoming
        $starts = $false
        try {
            $null = & (Join-Path $incoming 'reqloom.exe') --version 2>$null
            $starts = ($LASTEXITCODE -eq 0)
        } catch {
            $starts = $false
        }
        if (-not $starts) {
            Remove-Item -LiteralPath $incoming -Recurse -Force
            if ($createdInstallDir) { Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue }
            throw "reqloom install: reqloom $tag does not start on this system; nothing was installed. Build from source: $buildPage"
        }
        New-Item -ItemType Directory -Force -Path $binDir | Out-Null
        if (Test-Path -LiteralPath $versionDir) { Remove-Item -LiteralPath $versionDir -Recurse -Force }
        Move-Item -LiteralPath $incoming -Destination $versionDir
        Set-Content -LiteralPath (Join-Path $binDir 'reqloom.cmd') -Encoding Ascii -Value "@`"%~dp0..\$tag\reqloom.exe`" %*"

        Write-Host "Installed reqloom $tag in $versionDir"
        if ($env:REQLOOM_NO_PATH -eq '1') {
            Write-Host "PATH was left alone. The launcher is $binDir\reqloom.cmd"
        } else {
            # Read and written through the registry without expanding: the .NET
            # call returns %VAR% entries already expanded, and writing that back
            # would freeze them to today's folder names.
            $envKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
            try {
                $userPath = [string]$envKey.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                $entries = @($userPath.Split(';') | Where-Object { $_ })
                $present = $entries | Where-Object { [Environment]::ExpandEnvironmentVariables($_).TrimEnd('\') -ieq $binDir.TrimEnd('\') }
                if (-not $present) {
                    $kind = if ($envKey.GetValueNames() -contains 'Path') { $envKey.GetValueKind('Path') } else { [Microsoft.Win32.RegistryValueKind]::ExpandString }
                    $envKey.SetValue('Path', (($entries + $binDir) -join ';'), $kind)
                    # A registry write alone is not announced; setting any user
                    # variable through .NET tells running programs to reload.
                    [Environment]::SetEnvironmentVariable('REQLOOM_INSTALL_REFRESH', $null, 'User')
                    Write-Host "Added $binDir to your user PATH. Open a new terminal, then run: reqloom --version"
                } else {
                    Write-Host 'Run: reqloom --version'
                }
            } finally {
                $envKey.Close()
            }
        }
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# No exit here: under "irm | iex" it would close the user's terminal.
Install-Reqloom
