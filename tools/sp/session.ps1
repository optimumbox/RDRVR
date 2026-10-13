<#
.SYNOPSIS
    A flat test session: deploy this tree's build with the test switches and start the game (the player loads the save); or end it
    (close the game, put the release back, remove the test files).

.EXAMPLE
    tools\sp\session.ps1 -Start
    tools\sp\session.ps1 -Stop
#>
param(
    [switch] $Start,
    [switch] $Stop,
    [switch] $Install,
    [string] $GameDir = 'G:\Games\Red Dead Redemption',
    [string] $Shaders = 'C:\Games\RDRVR-src\research\stereo_all7',
    [string] $ExtraIni = ''
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$test = 'RDRVR.dev.ini', 'RDRVR_xr_runtime.txt', 'RDRVR_cmd.txt'

if ($Stop) {
    $p = Get-Process RDR -ErrorAction SilentlyContinue
    if ($p) {
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
if ($Install) {
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
ViSkipRecord=0
ViShaders=$Shaders
$ExtraIni
"@ | Set-Content (Join-Path $GameDir 'RDRVR.dev.ini') -Encoding ascii
    Remove-Item (Join-Path $GameDir 'RDRVR_xr_runtime.txt') -ErrorAction SilentlyContinue
    'installed for a VR session: start the game yourself ("tools\sp\session.ps1 -Stop" puts the release back)'
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

