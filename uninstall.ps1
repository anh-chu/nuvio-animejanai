<#
.SYNOPSIS
    Removes the Nuvio AnimeJaNai integration.

.DESCRIPTION
    Clears NUVIO_LIBMPV_PATH, NUVIO_MPV_CONFIG_DIR and NUVIO_MPV_REAL_LIBRARY
    from the user environment and deletes the install directory.

    Nuvio itself is untouched: its bridge and bundled libmpv are never
    modified, so removing this is enough to get the stock player back.
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'NuvioAnimeJaNai')
)

$ErrorActionPreference = 'Stop'

Write-Host "Removing Nuvio AnimeJaNai from $InstallDir" -ForegroundColor Cyan

foreach ($name in @('NUVIO_LIBMPV_PATH', 'NUVIO_MPV_CONFIG_DIR', 'NUVIO_MPV_REAL_LIBRARY')) {
    $current = [Environment]::GetEnvironmentVariable($name, 'User')
    if ($current) {
        [Environment]::SetEnvironmentVariable($name, $null, 'User')
        Write-Host "  removed $name" -ForegroundColor Green
    }
}

if (Test-Path -LiteralPath $InstallDir) {
    Remove-Item -LiteralPath $InstallDir -Recurse -Force
    Write-Host "  removed $InstallDir" -ForegroundColor Green
}

$running = Get-Process -Name 'Nuvio' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host ""
    Write-Host "Quit Nuvio completely (including the tray icon) to finish." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Done. Nuvio is back to its stock player." -ForegroundColor Green
