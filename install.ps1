<#
.SYNOPSIS
    Installs AnimeJaNai's neural upscaler into Nuvio Desktop's embedded player.

.DESCRIPTION
    Nuvio's player bridge starts libmpv with `config=no`, so mpv never reads
    mpv.conf, input.conf, scripts/ or shaders/. Upstream Nuvio has no setting
    for that, and patching the bridge means rebuilding it for every Nuvio
    release.

    This installer takes a different route. It points Nuvio's existing
    NUVIO_LIBMPV_PATH hook at a tiny shim (libmpv-2.dll) that forwards to
    AnimeJaNai's forked libmpv and rewrites exactly one call:

        mpv_set_option_string(mpv, "config", "no")
            -> config-dir=<this install>\config  +  config=yes

    The shim only depends on the stable libmpv C ABI, so it keeps working
    across Nuvio updates without rebuilding anything.

    Everything is installed under %LOCALAPPDATA%\NuvioAnimeJaNai. Three
    user-scope environment variables are set. No admin rights, no files in
    Program Files, no changes to %APPDATA%\mpv.

.PARAMETER InstallDir
    Install location. Default: %LOCALAPPDATA%\NuvioAnimeJaNai

.PARAMETER AnimeJaNaiVersion
    AnimeJaNai release to install. Default: 3.8.0

.PARAMETER UserConfig
    mpv.conf whose settings should be carried over into the generated config.
    Default: %APPDATA%\mpv\mpv.conf (ignored when it does not exist).

.PARAMETER TritonComponent
    TensorRT component to download (sm75/sm80/sm86/sm89/sm90/sm100/sm120/ptx).
    Default: auto-detected from nvidia-smi.

.PARAMETER WithRife
    Also download RIFE frame-interpolation models (~440 MB). Not needed for
    upscaling; AnimeJaNai works without it.

.PARAMETER KeepAnime4K
    Do not strip `glsl-shaders` profiles out of the copied mpv.conf. AnimeJaNai
    upscales in the filter chain, so Anime4K would then run on its 4K output.

.PARAMETER NoEnv
    Prepare the install but do not touch environment variables.

.PARAMETER Uninstall
    Remove the environment variables and the install directory.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1

.EXAMPLE
    .\install.ps1 -WithRife -KeepAnime4K
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'NuvioAnimeJaNai'),
    [string]$AnimeJaNaiVersion = '3.8.0',
    [string]$UserConfig = (Join-Path $env:APPDATA 'mpv\mpv.conf'),
    [ValidateSet('auto', 'sm75', 'sm80', 'sm86', 'sm89', 'sm90', 'sm100', 'sm120', 'ptx')]
    [string]$TritonComponent = 'auto',
    [switch]$WithRife,
    [switch]$KeepAnime4K,
    [switch]$NoEnv,
    [switch]$Force,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

$ReleaseBase = "https://github.com/the-database/mpv-AnimeJaNai/releases/download/$AnimeJaNaiVersion"
$ShimUrl = 'https://raw.githubusercontent.com/anh-chu/nuvio-animejanai/main/shim/libmpv-2.dll'
$ShimSha256 = '147859ea4b6854de19e2719d24c7ad6e428f7b3d4c19792f04e95090023d681c'

$CacheDir = Join-Path $InstallDir 'cache'
$PlayerDir = Join-Path $InstallDir 'libmpv'
$RealLib = Join-Path $PlayerDir 'libmpv-2.dll'
$ShimDir = Join-Path $InstallDir 'shim'
$ShimDll = Join-Path $ShimDir 'libmpv-2.dll'
$ConfigDir = Join-Path $InstallDir 'config'
$LogDir = Join-Path $InstallDir 'logs'
$DataDir = Join-Path $InstallDir 'animejanai'
$ManagedConf = Join-Path $InstallDir 'portable_config\mpv-animejanai.conf'

$EnvNames = @('NUVIO_LIBMPV_PATH', 'NUVIO_MPV_CONFIG_DIR', 'NUVIO_MPV_REAL_LIBRARY')

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

function Get-Utf8NoBom {
    return [System.Text.UTF8Encoding]::new($false)
}

function Write-TextFile {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Text, (Get-Utf8NoBom))
}

# Streams the file instead of loading it: AnimeJaNai's libmpv is ~120 MB and
# ReadAllBytes plus a string copy of it is needless memory pressure.
function Test-BinaryContainsAscii {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Needle
    )
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $chunkSize = 1MB
        $buffer = [byte[]]::new($chunkSize)
        $overlap = $Needle.Length - 1
        $carry = ''
        while (($read = $stream.Read($buffer, 0, $chunkSize)) -gt 0) {
            $text = $carry + [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
            if ($text.IndexOf($Needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                return $true
            }
            if ($overlap -gt 0 -and $text.Length -gt $overlap) {
                $carry = $text.Substring($text.Length - $overlap)
            } else {
                $carry = ''
            }
        }
        return $false
    } finally {
        $stream.Dispose()
    }
}

function Get-RemoteFile {
    param(
        [Parameter(Mandatory)][string]$Url,
        [Parameter(Mandatory)][string]$Path,
        [int]$MinBytes = 1MB,
        [string]$Sha256 = ''
    )

    if ((Test-Path -LiteralPath $Path) -and -not $Force) {
        $existing = (Get-Item -LiteralPath $Path).Length
        $hashOk = (-not $Sha256) -or
            ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ieq $Sha256)
        if (($existing -ge $MinBytes) -and $hashOk) {
            $size = if ($existing -ge 1MB) { "{0:N0} MB" -f ($existing / 1MB) } else { "{0:N0} KB" -f ($existing / 1KB) }
            Write-Note ("reusing {0} ({1})" -f (Split-Path -Leaf $Path), $size)
            return
        }
    }

    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    Write-Note ("downloading {0}" -f (Split-Path -Leaf $Path))
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        # -sS: no progress bar, but still report errors.
        & $curl.Source -L --fail -sS --retry 3 --retry-delay 2 -o $Path $Url
        if ($LASTEXITCODE -ne 0) {
            throw "curl.exe failed ($LASTEXITCODE) downloading $Url"
        }
    } else {
        Invoke-WebRequest -Uri $Url -OutFile $Path -UseBasicParsing
    }

    if (-not (Test-Path -LiteralPath $Path) -or (Get-Item -LiteralPath $Path).Length -lt $MinBytes) {
        throw "Download produced no usable file: $Url"
    }

    if ($Sha256) {
        $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $Sha256.ToLowerInvariant()) {
            Remove-Item -LiteralPath $Path -Force
            throw "SHA-256 mismatch for $Url`nexpected $($Sha256.ToLowerInvariant())`n     got $actual"
        }
    }
}

function Get-SevenZip {
    $local = Join-Path $CacheDir '7zr.exe'
    if (Test-Path -LiteralPath $local) {
        return $local
    }
    New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
    Write-Note "fetching 7zr.exe (standalone 7-Zip)"
    Invoke-WebRequest -Uri 'https://www.7-zip.org/a/7zr.exe' -OutFile $local -UseBasicParsing
    return $local
}

function Expand-Archive7z {
    param(
        [Parameter(Mandatory)][string]$Archive,
        [Parameter(Mandatory)][string]$Destination,
        [string[]]$Include
    )
    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    }
    $sevenZip = Get-SevenZip
    Write-Note ("extracting {0}" -f (Split-Path -Leaf $Archive))
    if ($Include) {
        & $sevenZip x $Archive @Include "-o$Destination" -y | Out-Null
    } else {
        & $sevenZip x $Archive "-o$Destination" -y | Out-Null
    }
    if ($LASTEXITCODE -ne 0) {
        throw "7zr failed ($LASTEXITCODE) extracting $Archive"
    }
}

# Pulls a single entry out of an archive and flattens it into $Destination.
# Used for libmpv-2.dll: the full package around it is ~560 MB of mpv.exe,
# mpvnet.exe and WPF runtime that Nuvio has no use for.
function Expand-ArchiveEntry {
    param(
        [Parameter(Mandatory)][string]$Archive,
        [Parameter(Mandatory)][string]$Entry,
        [Parameter(Mandatory)][string]$Destination
    )
    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    }
    $sevenZip = Get-SevenZip
    Write-Note ("extracting {0}" -f $Entry)
    & $sevenZip e $Archive $Entry "-o$Destination" -y | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "7zr failed ($LASTEXITCODE) extracting $Entry from $Archive"
    }
}

function Get-TensorRtComponent {
    if ($TritonComponent -ne 'auto') {
        return $TritonComponent
    }

    $smi = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
    if (-not $smi) {
        Write-Warn "nvidia-smi not found; falling back to the PTX component (JIT-compiled engines, slower first run)."
        return 'ptx'
    }

    # nvidia-smi writes the value; capture first, then take the first line.
    # (Piping straight into Select-Object -First can terminate the native
    # process early and leave $LASTEXITCODE non-zero.)
    $output = @(& $smi.Source --query-gpu=compute_cap --format=csv,noheader 2>$null)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -or $output.Count -eq 0 -or -not $output[0]) {
        Write-Warn "could not read compute capability; falling back to the PTX component."
        return 'ptx'
    }

    $capability = ([string]$output[0]).Trim()
    $map = @{
        '7.5' = 'sm75'
        '8.0' = 'sm80'
        '8.6' = 'sm86'
        '8.9' = 'sm89'
        '9.0' = 'sm90'
        '10.0' = 'sm100'
        '12.0' = 'sm120'
    }
    if ($map.ContainsKey($capability)) {
        Write-Note "NVIDIA compute capability $capability -> $($map[$capability])"
        return $map[$capability]
    }

    Write-Warn "no prebuilt TensorRT builder for compute capability $capability; using PTX."
    return 'ptx'
}

function Remove-ShaderProfiles {
    param([string]$Text)

    if (-not $Text) {
        return $Text
    }

    $lines = $Text -split "`r?`n"
    $output = New-Object System.Collections.Generic.List[string]
    $section = New-Object System.Collections.Generic.List[string]
    $sectionName = $null
    $sectionHasShaders = $false
    $dropped = New-Object System.Collections.Generic.List[string]

    function Flush-Section {
        if ($null -eq $sectionName) {
            foreach ($line in $section) { $output.Add($line) }
            return
        }
        if ($sectionHasShaders) {
            $dropped.Add($sectionName)
            return
        }
        foreach ($line in $section) { $output.Add($line) }
    }

    foreach ($line in $lines) {
        $match = [regex]::Match($line, '^\s*\[([^\]]+)\]\s*$')
        if ($match.Success) {
            Flush-Section
            $section.Clear()
            $sectionName = $match.Groups[1].Value.Trim()
            $sectionHasShaders = $false
            $section.Add($line) | Out-Null
            continue
        }
        if ($null -ne $sectionName -and $line -match '^\s*glsl-shaders?\s*=') {
            $sectionHasShaders = $true
        }
        if ($null -eq $sectionName) {
            $section.Add($line) | Out-Null
        } else {
            $section.Add($line) | Out-Null
        }
    }
    Flush-Section

    if ($dropped.Count -gt 0) {
        Write-Warn ("dropped shader profiles (AnimeJaNai already upscales): {0}" -f ($dropped -join ', '))
        Write-Note "pass -KeepAnime4K to keep them."
    }

    return ($output -join "`r`n")
}

function Write-MpvConfig {
    $userText = ''
    if ($UserConfig -and (Test-Path -LiteralPath $UserConfig)) {
        $userText = [System.IO.File]::ReadAllText($UserConfig)
        Write-Note "carrying over settings from $UserConfig"
    } else {
        Write-Note "no user mpv.conf found; using AnimeJaNai defaults"
    }

    if (-not $KeepAnime4K) {
        $userText = Remove-ShaderProfiles -Text $userText
    }

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.AppendLine("# ============================================================================")
    [void]$builder.AppendLine("#  Generated by install.ps1 (nuvio-animejanai)")
    [void]$builder.AppendLine("#  Re-run the installer to regenerate. Changes made here are overwritten.")
    [void]$builder.AppendLine("# ============================================================================")
    [void]$builder.AppendLine("")
    if ($userText.Trim()) {
        [void]$builder.AppendLine("# ---- settings carried over from your mpv.conf ------------------------------")
        [void]$builder.AppendLine($userText.TrimEnd())
        [void]$builder.AppendLine("")
    }

    [void]$builder.AppendLine("# ---- AnimeJaNai ------------------------------------------------------------")
    [void]$builder.AppendLine("# Managed defaults + the [upscale-on] profile that inserts the native filter.")
    [void]$builder.AppendLine("include=`"$ManagedConf`"")
    [void]$builder.AppendLine("profile=animejanai")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("# Nuvio does not forward keystrokes to mpv, so slots are switched over IPC:")
    [void]$builder.AppendLine("#     .\aji-slot.ps1 1002")
    [void]$builder.AppendLine("input-ipc-server=\\.\pipe\nuvio-animejanai")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("# AnimeJaNai outputs 4K; keep the downscale to a smaller panel sharp.")
    [void]$builder.AppendLine("dscale=ewa_lanczos")
    [void]$builder.AppendLine("dscale-antiring=0.75")
    [void]$builder.AppendLine("correct-downscaling=yes")
    [void]$builder.AppendLine("linear-downscaling=yes")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("# Diagnostics: the embedded player has no console.")
    [void]$builder.AppendLine("log-file=`"$LogDir\mpv.log`"")
    [void]$builder.AppendLine("msg-level=all=info")

    Write-TextFile -Path (Join-Path $ConfigDir 'mpv.conf') -Text $builder.ToString()

    # input.conf: mpv only ever autoloads this name, so the slot bindings from
    # AnimeJaNai's input-animejanai.conf have to live here. Nuvio swallows keys,
    # but these still work over IPC / if Nuvio ever forwards input.
    $inputConf = @(
        '# AnimeJaNai slot switching (also reachable over IPC: .\aji-slot.ps1 N)',
        '!        script-message aji-slot 1001   # Quality',
        '@        script-message aji-slot 1002   # Balanced',
        'SHARP    script-message aji-slot 1003   # Performance',
        'Ctrl+0   script-message aji-slot 0      # Off',
        'Ctrl+1   script-message aji-slot 1',
        'Ctrl+2   script-message aji-slot 2',
        'Ctrl+3   script-message aji-slot 3',
        'Ctrl+4   script-message aji-slot 4',
        'Ctrl+5   script-message aji-slot 5',
        'Ctrl+6   script-message aji-slot 6',
        'Ctrl+7   script-message aji-slot 7',
        'Ctrl+8   script-message aji-slot 8',
        'Ctrl+9   script-message aji-slot 9'
    ) -join "`r`n"
    Write-TextFile -Path (Join-Path $ConfigDir 'input.conf') -Text $inputConf
}

function Install-ScriptsAndShaders {
    param([Parameter(Mandatory)][string]$OverlayDir)

    $sourceScripts = Join-Path $OverlayDir 'portable_config\scripts'
    $targetScripts = Join-Path $ConfigDir 'scripts'
    if (Test-Path -LiteralPath $sourceScripts) {
        New-Item -ItemType Directory -Force -Path $targetScripts | Out-Null
        # Skip the self-updater: it wants AnimeJaNaiUpdater.exe at the install
        # root, which this installer deliberately does not ship (re-run
        # install.ps1 to move to a newer AnimeJaNai version instead).
        Get-ChildItem -LiteralPath $sourceScripts -Filter '*.lua' |
            Where-Object { $_.Name -ne 'animejanai_update.lua' } |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $targetScripts -Force }
        $count = (Get-ChildItem -LiteralPath $targetScripts -Filter '*.lua').Count
        Write-Ok "$count Lua script(s) copied into the mpv config dir"
    } else {
        Write-Warn "AnimeJaNai scripts/ not found in the overlay; is the archive complete?"
    }

    $sourceShaders = Join-Path $OverlayDir 'portable_config\shaders'
    $targetShaders = Join-Path $ConfigDir 'shaders'
    if (Test-Path -LiteralPath $sourceShaders) {
        New-Item -ItemType Directory -Force -Path $targetShaders | Out-Null
        Copy-Item -Path (Join-Path $sourceShaders '*') -Destination $targetShaders -Recurse -Force
        Write-Ok "AnimeJaNai shaders copied into the mpv config dir"
    }
}

function Set-NuvioEnvironment {
    $values = @{
        NUVIO_LIBMPV_PATH = $ShimDll
        NUVIO_MPV_CONFIG_DIR = $ConfigDir
        NUVIO_MPV_REAL_LIBRARY = $RealLib
    }
    foreach ($name in $values.Keys) {
        [Environment]::SetEnvironmentVariable($name, $values[$name], 'User')
        Set-Item -Path "Env:$name" -Value $values[$name]
        Write-Ok ("{0} = {1}" -f $name, $values[$name])
    }
}

function Remove-NuvioEnvironment {
    foreach ($name in $EnvNames) {
        $current = [Environment]::GetEnvironmentVariable($name, 'User')
        if ($current) {
            [Environment]::SetEnvironmentVariable($name, $null, 'User')
            Write-Ok "removed $name"
        }
    }
}

function Assert-NuvioVersionSupport {
    $candidates = @(
        'C:\Program Files\Nuvio\Nuvio.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\Nuvio\Nuvio.exe')
    )
    $exe = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $exe) {
        Write-Warn "Nuvio.exe not found in the usual locations; installing anyway."
        return
    }
    $version = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
    Write-Note "Nuvio at $exe (version $version)"

    $running = Get-Process -Name 'Nuvio' -ErrorAction SilentlyContinue
    if ($running) {
        Write-Warn "Nuvio is running; quit it completely (tray icon too) before playing, or the new environment will not be picked up."
    }
}

# ---------------------------------------------------------------------------

if ($Uninstall) {
    Write-Step "Uninstalling Nuvio AnimeJaNai"
    Remove-NuvioEnvironment
    if (Test-Path -LiteralPath $InstallDir) {
        Write-Note "removing $InstallDir"
        Remove-Item -LiteralPath $InstallDir -Recurse -Force
    }
    Write-Host ""
    Write-Host "Done. Nuvio is back to its stock player." -ForegroundColor Green
    exit 0
}

Write-Step "Nuvio AnimeJaNai installer"
Write-Note "install dir : $InstallDir"
Write-Note "AnimeJaNai  : $AnimeJaNaiVersion"

Assert-NuvioVersionSupport

foreach ($dir in @($CacheDir, $PlayerDir, $ShimDir, $ConfigDir, $LogDir)) {
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
}

# --- 1. AnimeJaNai player components ---------------------------------------

Write-Step "Downloading AnimeJaNai $AnimeJaNaiVersion"

$fullPackage = Join-Path $CacheDir "mpv-upscale-2x_animejanai-full-package-$AnimeJaNaiVersion.7z.001"
$overlay = Join-Path $CacheDir "mpv-upscale-2x_animejanai-overlay-$AnimeJaNaiVersion.7z"
$runtime = Join-Path $CacheDir 'component-trt-runtime.7z'

Get-RemoteFile -Url "$ReleaseBase/mpv-upscale-2x_animejanai-full-package-$AnimeJaNaiVersion.7z.001" -Path $fullPackage
Get-RemoteFile -Url "$ReleaseBase/mpv-upscale-2x_animejanai-overlay-$AnimeJaNaiVersion.7z" -Path $overlay
Get-RemoteFile -Url "$ReleaseBase/component-trt-runtime.7z" -Path $runtime

$component = Get-TensorRtComponent
$builder = Join-Path $CacheDir "component-trt-$component.7z"
Get-RemoteFile -Url "$ReleaseBase/component-trt-$component.7z" -Path $builder

$rifeArchive = $null
if ($WithRife) {
    $rifeArchive = Join-Path $CacheDir 'component-rife.7z'
    Get-RemoteFile -Url "$ReleaseBase/component-rife.7z" -Path $rifeArchive
}

Write-Step "Extracting player"

$packageExtract = Join-Path $CacheDir 'libmpv'
$sourceLib = Join-Path $packageExtract 'libmpv-2.dll'
if ($Force -or -not (Test-Path -LiteralPath $sourceLib)) {
    Expand-ArchiveEntry -Archive $fullPackage -Entry 'mpv-upscale-2x_animejanai-*\libmpv-2.dll' -Destination $packageExtract
}
if (-not (Test-Path -LiteralPath $sourceLib)) {
    throw "libmpv-2.dll could not be extracted from $fullPackage"
}
Copy-Item -LiteralPath $sourceLib -Destination $RealLib -Force
Write-Ok ("libmpv-2.dll ({0:N0} MB)" -f ((Get-Item -LiteralPath $RealLib).Length / 1MB))

# The overlay also carries the full player (mpvnet.exe, WPF runtime, its own
# mpv.conf/input.conf). Extract it aside and take only the animejanai data plus
# the managed mpv-animejanai.conf, so the install dir holds no stray binaries
# and no second mpv.conf competing with the generated one.
$overlayExtract = Join-Path $CacheDir 'overlay'
$overlayMarker = Join-Path $CacheDir "overlay-$AnimeJaNaiVersion.done"
if ($Force -or -not (Test-Path -LiteralPath $overlayMarker)) {
    Expand-Archive7z -Archive $overlay -Destination $overlayExtract
    Write-TextFile -Path $overlayMarker -Text $AnimeJaNaiVersion
} else {
    Write-Note "overlay already extracted"
}
if (-not (Test-Path -LiteralPath (Join-Path $overlayExtract 'animejanai'))) {
    throw "Unexpected overlay layout under $overlayExtract (no animejanai/ folder)"
}
# Merge rather than replace: TensorRT engine caches and logs live under
# animejanai/ and survive an upgrade.
function Copy-DirectoryTree {
    param([string]$Source, [string]$Destination)
    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    }
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
}

Copy-DirectoryTree -Source (Join-Path $overlayExtract 'animejanai') -Destination $DataDir
$managedSource = Join-Path $overlayExtract 'portable_config\mpv-animejanai.conf'
if (-not (Test-Path -LiteralPath $managedSource)) {
    throw "Managed config missing from the overlay: $managedSource"
}
$managedParent = Split-Path -Parent $ManagedConf
if (-not (Test-Path -LiteralPath $managedParent)) {
    New-Item -ItemType Directory -Force -Path $managedParent | Out-Null
}
Copy-Item -LiteralPath $managedSource -Destination $ManagedConf -Force
Write-Ok "AnimeJaNai data and managed config installed"

Expand-Archive7z -Archive $runtime -Destination $InstallDir

$builderExtractMarker = Join-Path $CacheDir "builder-$component.done"
if ($Force -or -not (Test-Path -LiteralPath $builderExtractMarker)) {
    Expand-Archive7z -Archive $builder -Destination $InstallDir
    Write-TextFile -Path $builderExtractMarker -Text $component
} else {
    Write-Note "TensorRT $component already extracted"
}

if ($rifeArchive) {
    Expand-Archive7z -Archive $rifeArchive -Destination $InstallDir
}

if (-not (Test-Path -LiteralPath (Join-Path $DataDir 'inference\aji.dll'))) {
    throw "AnimeJaNai inference runtime missing after extraction: $DataDir\inference\aji.dll"
}
if (-not (Test-Path -LiteralPath $ManagedConf)) {
    throw "AnimeJaNai managed config missing: $ManagedConf"
}

$libHasFilter = Test-BinaryContainsAscii -Path $RealLib -Needle 'animejanai'
if (-not $libHasFilter) {
    throw "The libmpv-2.dll from AnimeJaNai does not contain the native 'animejanai' filter. Wrong package?"
}
Write-Ok "native animejanai filter found in libmpv-2.dll"

# --- 2. shim ---------------------------------------------------------------

Write-Step "Installing the libmpv shim"
Get-RemoteFile -Url $ShimUrl -Path $ShimDll -MinBytes 16KB -Sha256 $ShimSha256
$shimSize = (Get-Item -LiteralPath $ShimDll).Length
if ($shimSize -lt 4096) {
    throw "Shim looks truncated ($shimSize bytes): $ShimDll"
}
Write-Ok ("shim {0:N0} bytes" -f $shimSize)

# --- 3. config -------------------------------------------------------------

Write-Step "Writing mpv config"
Install-ScriptsAndShaders -OverlayDir $overlayExtract
Write-MpvConfig
Write-Ok "config dir: $ConfigDir"

# Keep the helper scripts next to the install, so they still work after the
# folder the installer was run from is gone.
if ($PSScriptRoot) {
    foreach ($helper in @('aji-slot.ps1', 'uninstall.ps1')) {
        $source = Join-Path $PSScriptRoot $helper
        $target = Join-Path $InstallDir $helper
        if ((Test-Path -LiteralPath $source) -and ($source -ne $target)) {
            Copy-Item -LiteralPath $source -Destination $target -Force
            Write-Ok "$helper -> $target"
        }
    }
}

# --- 4. environment --------------------------------------------------------

if ($NoEnv) {
    Write-Step "Skipping environment variables (-NoEnv)"
    Write-Note "set these yourself, then restart Nuvio:"
    Write-Note "  NUVIO_LIBMPV_PATH      = $ShimDll"
    Write-Note "  NUVIO_MPV_CONFIG_DIR   = $ConfigDir"
    Write-Note "  NUVIO_MPV_REAL_LIBRARY = $RealLib"
} else {
    Write-Step "Setting user environment variables"
    Set-NuvioEnvironment
}

# --- 5. done ---------------------------------------------------------------

Write-Host ""
Write-Host "Installed." -ForegroundColor Green
Write-Host ""
Write-Host "  Fully quit Nuvio (including the tray icon), then play something." -ForegroundColor White
Write-Host ""
Write-Host "  Check it is active:" -ForegroundColor White
Write-Host "    Get-Content '$LogDir\mpv.log' -Tail 40"
Write-Host "    look for:  [animejanai] ... slot=1002"
Write-Host ""
Write-Host "  Switch slots while playing:" -ForegroundColor White
Write-Host "    & '$InstallDir\aji-slot.ps1' 1001   # Quality"
Write-Host "    & '$InstallDir\aji-slot.ps1' 1002   # Balanced (default)"
Write-Host "    & '$InstallDir\aji-slot.ps1' 1003   # Performance"
Write-Host "    & '$InstallDir\aji-slot.ps1' 0      # Off (bypass)"
Write-Host ""
Write-Host "  After a Nuvio update: nothing to do. The shim is version-independent." -ForegroundColor White
Write-Host "  To remove: & '$InstallDir\uninstall.ps1'" -ForegroundColor White
Write-Host ""
