<#
.SYNOPSIS
    Makes Nuvio Desktop read your own mpv.conf, without AnimeJaNai.

.DESCRIPTION
    Nuvio's player bridge starts libmpv with `config=no`, so mpv ignores
    %APPDATA%\mpv\mpv.conf. This script installs a tiny per-user shim that
    Nuvio loads through its existing NUVIO_LIBMPV_PATH hook and rewrites that
    one call into `config-dir=<ConfigDir>` + `config=yes`.

    The shim finds the real libmpv next to the running player_bridge.dll, so
    Nuvio updates are picked up automatically (including changes to Nuvio's
    content-hash cache directory). It does not patch Nuvio, does not need
    administrator rights, and does not install AnimeJaNai, models, TensorRT,
    or any package under Program Files.

    Only two user environment variables are set:
        NUVIO_LIBMPV_PATH       shim path
        NUVIO_MPV_CONFIG_DIR    your mpv config directory
    The old fixed-path variable NUVIO_MPV_REAL_LIBRARY is cleared so the shim
    uses auto-discovery.

.PARAMETER InstallDir
    Where to put the shim. Default: %LOCALAPPDATA%\NuvioMpvConfig

.PARAMETER ConfigDir
    mpv config directory to load. Default: %APPDATA%\mpv

.PARAMETER NoEnv
    Install the shim but do not change environment variables.

.PARAMETER Force
    Replace an existing NUVIO_MPV_REAL_LIBRARY that points to a custom
    libmpv (for example an AnimeJaNai install). Without this switch the
    script stops instead of silently replacing it.

.PARAMETER Uninstall
    Remove the shim and the two environment variables this script manages.
    The mpv config directory is left untouched.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install-config-only.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install-config-only.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'NuvioMpvConfig'),
    [string]$ConfigDir = (Join-Path $env:APPDATA 'mpv'),
    [switch]$NoEnv,
    [switch]$Force,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

$ShimUrl = 'https://raw.githubusercontent.com/anh-chu/nuvio-animejanai/main/shim/libmpv-2.dll'
$ShimSha256 = 'a59654d91c542c7a1bc76aef5384a12882b839af452a97e73f18008691b2f91b'
$ShimDll = Join-Path $InstallDir 'libmpv-2.dll'

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Note {
    param([string]$Message)
    Write-Host "    $Message" -ForegroundColor DarkGray
}

function Write-Ok {
    param([string]$Message)
    Write-Host "    $Message" -ForegroundColor Green
}

function Write-Warn {
    param([string]$Message)
    Write-Host "    $Message" -ForegroundColor Yellow
}

function Remove-UserVariable {
    param([string]$Name)
    [Environment]::SetEnvironmentVariable($Name, $null, 'User')
}

if ($Uninstall) {
    Write-Step 'Removing the mpv config shim'
    $libPath = [Environment]::GetEnvironmentVariable('NUVIO_LIBMPV_PATH', 'User')
    $configPath = [Environment]::GetEnvironmentVariable('NUVIO_MPV_CONFIG_DIR', 'User')

    $ownsLibPath = $libPath -and $libPath -ieq $ShimDll
    if ($ownsLibPath) {
        Remove-UserVariable 'NUVIO_LIBMPV_PATH'
        Write-Ok "removed NUVIO_LIBMPV_PATH"
    } else {
        Write-Note "NUVIO_LIBMPV_PATH left alone (not this install)"
    }

    # Only remove the config pointer when this shim was the active library.
    # A -NoEnv install (or a different active install) must not touch the
    # user's existing environment.
    if ($ownsLibPath -and $configPath -and $configPath -ieq $ConfigDir) {
        Remove-UserVariable 'NUVIO_MPV_CONFIG_DIR'
        Write-Ok "removed NUVIO_MPV_CONFIG_DIR"
    } elseif ($configPath) {
        Write-Note "NUVIO_MPV_CONFIG_DIR left alone (this install is not active)"
    }

    if (Test-Path -LiteralPath $InstallDir) {
        Remove-Item -LiteralPath $InstallDir -Recurse -Force
        Write-Ok "removed $InstallDir"
    }

    Write-Step 'Done'
    Write-Note 'Your mpv config directory was not changed.'
    Write-Note 'Fully quit Nuvio (including the tray icon), then start it again.'
    return
}

Write-Step 'Installing the libmpv config shim'
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
if (-not (Test-Path -LiteralPath $ConfigDir)) {
    New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
    Write-Warn "created empty config directory: $ConfigDir"
}
if (-not (Test-Path -LiteralPath (Join-Path $ConfigDir 'mpv.conf'))) {
    Write-Warn "no mpv.conf in $ConfigDir yet; mpv will use its defaults until you add one"
}

Invoke-WebRequest -Uri $ShimUrl -OutFile $ShimDll -UseBasicParsing
$actual = (Get-FileHash -LiteralPath $ShimDll -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $ShimSha256) {
    Remove-Item -LiteralPath $ShimDll -Force -ErrorAction SilentlyContinue
    throw "SHA-256 mismatch for the shim.`nexpected $ShimSha256`n     got $actual"
}
Write-Ok ("shim {0:N0} bytes, sha256 verified" -f (Get-Item -LiteralPath $ShimDll).Length)

if (-not $NoEnv) {
    $existingReal = [Environment]::GetEnvironmentVariable('NUVIO_MPV_REAL_LIBRARY', 'User')
    if ($existingReal -and $existingReal -notmatch '\\Nuvio\\Cache\\native-player-bridge\\') {
        if ((Test-Path -LiteralPath $existingReal) -and -not $Force) {
            throw "NUVIO_MPV_REAL_LIBRARY points to an existing custom libmpv:`n  $existingReal`nThis looks like an AnimeJaNai or other custom install. Re-run with -Force to replace it, or use install.ps1 for AnimeJaNai."
        }
        Write-Warn "NUVIO_MPV_REAL_LIBRARY was set to $existingReal"
        Write-Warn 'The config-only shim clears it and uses auto-discovery.'
    }

    Write-Step 'Setting user environment variables'
    [Environment]::SetEnvironmentVariable('NUVIO_LIBMPV_PATH', $ShimDll, 'User')
    [Environment]::SetEnvironmentVariable('NUVIO_MPV_CONFIG_DIR', $ConfigDir, 'User')
    Remove-UserVariable 'NUVIO_MPV_REAL_LIBRARY'
    Write-Ok "NUVIO_LIBMPV_PATH      = $ShimDll"
    Write-Ok "NUVIO_MPV_CONFIG_DIR   = $ConfigDir"
    Write-Ok 'NUVIO_MPV_REAL_LIBRARY = <cleared; auto-discovery>'
} else {
    Write-Step 'Skipping environment variables (-NoEnv)'
}

Write-Step 'Done'
Write-Note 'No files under Program Files were changed. No admin rights needed.'
Write-Note 'Fully quit Nuvio (including the tray icon), then start it again.'
Write-Note 'Check the player with: Get-Content "$env:APPDATA\mpv\mpv.log" -Tail 40'
