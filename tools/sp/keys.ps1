<#
.SYNOPSIS
    Bring the game's window to the front and send it keystrokes (the front-end menu takes no gamepad from the mod).

.EXAMPLE
    tools\sp\keys.ps1 '{ENTER}'
    tools\sp\keys.ps1 '{DOWN}','{ENTER}' -DelayMs 800
#>
param(
    [Parameter(Mandatory)] [string[]] $Keys,
    [int] $DelayMs = 500
)
$ErrorActionPreference = 'Stop'
$p = Get-Process -Name RDR -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { throw 'RDR.exe is not running' }
$sh = New-Object -ComObject WScript.Shell
if (-not $sh.AppActivate($p.Id)) { throw 'could not bring the game window to the front' }
Start-Sleep -Milliseconds 300
foreach ($k in $Keys) {
    $sh.SendKeys($k)
    Start-Sleep -Milliseconds $DelayMs
}
"sent $($Keys -join ' ')"
