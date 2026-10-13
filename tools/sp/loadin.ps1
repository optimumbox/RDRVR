<#
.SYNOPSIS
    Press Enter in the game until it is in the world (the menus and the save's load), then stop.

    "In the world", asked of the game's scripts through the test channel, twice -IntervalSec apart without a key press:
      - the player's actor exists and has a position (pos: GET_PLAYER_ACTOR, GET_POSITION),
      - the game is not paused (IS_GAME_PAUSED 0) and the HUD is not faded (HUD_IS_FADED 0: loading, fades).
    The menus fail at least one of them (no script tick, no actor, paused, or faded). Not the pre-pass regions: the
    main menu renders a scene too (the 2026-10-13 session stopped at the menu that way).

.EXAMPLE
    tools\sp\loadin.ps1
#>
param(
    [int] $TimeoutSec = 300,
    [int] $IntervalSec = 3
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Ask([string] $cmd) {
    try { return [string](& "$here\rdrcmd.ps1" $cmd 2>$null) } catch { return 'ERROR' }
}
function InWorld {
    $p = Ask 'pos'
    if ($p -notmatch '^actor (\d+) value .* vec \(([-\d.]+) ([-\d.]+) ([-\d.]+)\)') { return "no actor ($p)" }
    if ([int64]$Matches[1] -le 0) { return "no actor ($p)" }
    if ([double]$Matches[2] -eq 0 -and [double]$Matches[3] -eq 0 -and [double]$Matches[4] -eq 0) { return 'actor at the origin' }
    $paused = Ask 'native 57246C02'   # IS_GAME_PAUSED
    if ($paused -notmatch '\(int 0,') { return "paused ($paused)" }
    $faded = Ask 'native 4EFFFC06'    # HUD_IS_FADED
    if ($faded -notmatch '\(int 0,') { return "faded ($faded)" }
    return ''
}
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$good = 0
$why = ''
while ((Get-Date) -lt $deadline) {
    $why = InWorld
    if ($why -eq '') {
        if (++$good -ge 2) { 'in the world'; return }
        Start-Sleep -Seconds $IntervalSec
        continue
    }
    $good = 0
    "  not yet: $why"
    try { & "$here\keys.ps1" '{ENTER}' -DelayMs 200 | Out-Null } catch { "  Enter not sent: $_" }
    Start-Sleep -Seconds $IntervalSec
}
throw "not in the world after $TimeoutSec s (last: $why)"
