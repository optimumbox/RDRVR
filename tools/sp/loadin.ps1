<#
.SYNOPSIS
    Press Enter in the game until it is in the world (the menus and the save's load), then stop.

    "In the world": the pre-pass regions (prepass status) grow by more than -MinRegions in -IntervalSec seconds without a
    key press (the world renders a region a frame; the menus none). Times out after -TimeoutSec.

.EXAMPLE
    tools\sp\loadin.ps1
#>
param(
    [int] $TimeoutSec = 240,
    [int] $IntervalSec = 3,
    [int] $MinRegions = 60
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Regions {
    try {
        $s = & "$here\rdrcmd.ps1" 'prepass' 2>$null
        if ($s -match 'regions (\d+)') { return [int64]$Matches[1] }
    } catch {}
    return -1
}
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$quiet = 0
while ((Get-Date) -lt $deadline) {
    $a = Regions
    Start-Sleep -Seconds $IntervalSec
    $b = Regions
    if ($a -ge 0 -and $b - $a -gt $MinRegions) {
        if (++$quiet -ge 2) { "in the world (regions $b)"; return }
        continue  # (one more quiet interval: a menu scene can render briefly)
    }
    $quiet = 0
    try { & "$here\keys.ps1" '{ENTER}' -DelayMs 200 | Out-Null } catch {}
}
throw "not in the world after $TimeoutSec s"
