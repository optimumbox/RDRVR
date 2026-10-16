<#
.SYNOPSIS
    A flat test session: deploy this tree's build with the test switches and start the game (the player loads the save); or end it
    (close the game, put the release back, remove the test files).

.EXAMPLE
    tools\sp\session.ps1 -Start
    tools\sp\session.ps1 -Stop
    tools\sp\session.ps1 -Vr       # the headset config, and the game started (SteamVR running)
#>
param(
    [switch] $Start,
    [switch] $Stop,
    [switch] $Install,
    [switch] $Vr,
    [string] $GameDir = 'G:\Games\Red Dead Redemption',
    [string] $Shaders = 'C:\Games\RDRVR-src\research\stereo_all19',
    [string] $ExtraIni = ''
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$test = 'RDRVR.dev.ini', 'RDRVR_xr_runtime.txt', 'RDRVR_cmd.txt'

if ($Stop) {
    $p = Get-Process RDR -ErrorAction SilentlyContinue
    if ($p) {
        # the render-size boot sentinel: a start at the chosen size that ends within two minutes (this close is not a
        # quit) blocks that size for the player's next start (2026-10-13: twice, the headset ran at 2560x1440). Wait for
        # it to clear (two minutes at the size) before closing.
        $rc = Join-Path $here 'rdrcmd.ps1'
        for ($i = 0; $i -lt 40; $i++) {
            $st = try { [string](& $rc 'renderres' 2>$null) } catch { '' }
            if ($st -notmatch 'sentinel pending') { break }
            if ($i -eq 0) { 'waiting for the render-size sentinel to clear (two minutes at the size) before closing' }
            Start-Sleep 5
        }
        $p.CloseMainWindow() | Out-Null
        if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
    }
    Start-Sleep 3
    Get-Process crashpad_handler -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$GameDir\*" } | Stop-Process -Force
    & (Join-Path $here 'deploy.ps1') -GameDir $GameDir -Restore
    foreach ($f in $test) { Remove-Item (Join-Path $GameDir $f) -ErrorAction SilentlyContinue }
    'session ended: the release is back'
    return
}
if ($Install -or $Vr) {
    # a headset session the player starts: this build, single-pass stereo on from the start (the view-instanced main pass,
    # the second scene recorded without draws), the OpenXR runtime as configured (no flat override); "-Stop" undoes it
    & (Join-Path $here 'deploy.ps1') -GameDir $GameDir
    @"
; RDRVR development build: single-pass stereo for a headset session (delete this file to go back to normal)
[Stereo]
PrepassArrays=1
PrepassViews=2
ViPrepass=1
ViOn=1
ViMain=1
ViSkipRecord=1
ViShaders=$Shaders
$ExtraIni
"@ | Set-Content (Join-Path $GameDir 'RDRVR.dev.ini') -Encoding ascii
    Remove-Item (Join-Path $GameDir 'RDRVR_xr_runtime.txt') -ErrorAction SilentlyContinue
    if (-not $Vr) {
        'installed for a VR session: start the game yourself ("tools\sp\session.ps1 -Stop" puts the release back)'
        return
    }
    # -Vr: started here too (SteamVR running, the headset connected), back at the menu like -Start
    $log = Join-Path $GameDir 'RDRVR.log'
    $t0 = Get-Date
    Start-Process (Join-Path $GameDir 'PlayRDR.exe') -WorkingDirectory $GameDir
    do { Start-Sleep 2 } until ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and
                                (Select-String -Path $log -Pattern '\[vi\] view instancing tier' -Quiet))
    'game launched (VR): at the menu'
    return
}
if ($Start) {
    & (Join-Path $here 'deploy.ps1') -GameDir $GameDir
    @"
; RDRVR development build test switches (delete this file to go back to normal)
[Stereo]
PrepassArrays=1
PrepassViews=2
ViPrepass=1
ViOn=0
ViShaders=$Shaders
$ExtraIni
"@ | Set-Content (Join-Path $GameDir 'RDRVR.dev.ini') -Encoding ascii
    Set-Content (Join-Path $GameDir 'RDRVR_xr_runtime.txt') 'C:\none\flat_test_no_runtime.json' -Encoding ascii  # flat
    $log = Join-Path $GameDir 'RDRVR.log'
    $t0 = Get-Date
    Start-Process (Join-Path $GameDir 'PlayRDR.exe') -WorkingDirectory $GameDir
    # the front end: the mod's device is up, then a few seconds for the menu
    do { Start-Sleep 2 } until ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $t0 -and
                                (Select-String -Path $log -Pattern '\[vi\] view instancing tier' -Quiet))
    'game launched (flat): at the menu; the player loads the save'
}

