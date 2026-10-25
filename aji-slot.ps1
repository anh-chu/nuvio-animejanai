<#
.SYNOPSIS
    Switch AnimeJaNai upscaling slots in a running Nuvio player.

.DESCRIPTION
    Nuvio's embedded player sets input-vo-keyboard=no and only handles F11 in
    its own window procedure, so mpv never sees keystrokes inside Nuvio. The
    AnimeJaNai keybinds (Shift+1, Ctrl+J, ...) therefore do nothing there.

    The installer adds `input-ipc-server=\\.\pipe\nuvio-animejanai` to mpv.conf,
    which gives us the same control channel mpv's own IPC clients use. This
    script sends AnimeJaNai's `aji-slot` script message over that pipe, which
    is exactly what its input.conf bindings would have sent.

.PARAMETER Slot
    0      bypass (upscaling off)
    1-9    custom profiles from animejanai\animejanai.conf
    1001   built-in Quality
    1002   built-in Balanced (default)
    1003   built-in Performance

.PARAMETER PipeName
    mpv IPC pipe name. Default: nuvio-animejanai

.EXAMPLE
    .\aji-slot.ps1 1001

.EXAMPLE
    .\aji-slot.ps1 0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '1001', '1002', '1003')]
    [string]$Slot,
    [string]$PipeName = 'nuvio-animejanai'
)

$ErrorActionPreference = 'Stop'

$labels = @{
    '0' = 'Off (bypass)'
    '1001' = 'Quality'
    '1002' = 'Balanced'
    '1003' = 'Performance'
}
$label = if ($labels.ContainsKey($Slot)) { $labels[$Slot] } else { "custom profile $Slot" }

$payload = @{ command = @('script-message', 'aji-slot', $Slot) } | ConvertTo-Json -Compress

$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
    '.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut)

try {
    $pipe.Connect(2000)
} catch {
    $pipe.Dispose()
    Write-Host "Could not connect to mpv IPC pipe '$PipeName'." -ForegroundColor Red
    Write-Host "Is Nuvio running and playing a video? Start playback, then retry." -ForegroundColor Yellow
    exit 1
}

try {
    $writer = New-Object System.IO.StreamWriter($pipe)
    $writer.AutoFlush = $true
    $writer.WriteLine($payload)
    $writer.Dispose()
} finally {
    $pipe.Dispose()
}

Write-Host "AnimeJaNai slot -> $Slot ($label)" -ForegroundColor Green
