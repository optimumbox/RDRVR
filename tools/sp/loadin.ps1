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
    # paused / faded / stalled / no scene as the core's mode watcher sees them (its natives polled on the script
    # tick: "mode" ends "now: <flags>"). A direct "native" call read paused and faded 1 in the world in a headset
    # session (2026-10-13) while the watcher, the HUD and the stereo view all said gameplay.
    $mode = Ask 'mode'
    if ($mode -notmatch '\| now: ') { return "no mode status ($mode)" }
    $now = ($mode -split '\| now: ', 2)[1].Trim()
    if ($now -ne '' -and $now -ne 'cutscene') { return "not in play ($now)" }
    $cut = Ask 'cutscene'             # CUTSCENE_MANAGER_IS_CUTSCENE_PLAYING as the core watches it (Enter skips one); never
    if ($cut -ne 'cutscene 0') { return "cutscene ($cut)" }  # through "native": that hash is not callable so (crashed)
    return ''
}
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$good = 0
$why = ''
while ((Get-Date) -lt $deadline) {
    $why = InWorld
    if ($why -eq '') {
        if (++$good -ge 4) { 'in the world'; return }  # (4 x 3 s: the gaps between a cutscene's shots read as the world)
        Start-Sleep -Seconds $IntervalSec
        continue
    }
    $good = 0
    "  not yet: $why"
    # a cutscene takes a few presses to skip (the user, 2026-10-13); a menu one
    $keys = if ($why -like 'cutscene*') { @('{ENTER}', '{ENTER}', '{ENTER}', '{ENTER}') } else { @('{ENTER}') }
    try { & "$here\keys.ps1" $keys -DelayMs 300 | Out-Null } catch { "  Enter not sent: $_" }
    Start-Sleep -Seconds $IntervalSec
}
throw "not in the world after $TimeoutSec s (last: $why)"
