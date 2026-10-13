<#
.SYNOPSIS
    Put this tree's build into the game folder, or put the release back.

.DESCRIPTION
    The first deploy keeps the installed release's dinput8.dll and RDRVR.Gameplay.red in
    <game>\RDRVR_release_keep (never overwritten afterwards). -Restore copies them back.
    Refuses while the game or its crash reporter runs.

.EXAMPLE
    tools\sp\deploy.ps1
    tools\sp\deploy.ps1 -Restore
#>
param(
    [string] $GameDir = 'G:\Games\Red Dead Redemption',
    [switch] $Restore
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$files = 'dinput8.dll', 'RDRVR.Gameplay.red'
$keep = Join-Path $GameDir 'RDRVR_release_keep'

if (Get-Process -Name RDR, crashpad_handler -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($GameDir, [StringComparison]::OrdinalIgnoreCase) }) {
    throw 'The game (or its crash reporter) is running: quit it first.'
}
if (-not (Test-Path $keep)) {
    New-Item -ItemType Directory $keep | Out-Null
    foreach ($f in $files) { Copy-Item (Join-Path $GameDir $f) $keep }
    Write-Host "kept the release's files in $keep"
}
$src = if ($Restore) { $keep } else { Join-Path $root 'build' }
foreach ($f in $files) {
    Copy-Item (Join-Path $src $f) $GameDir -Force
    $h = (Get-FileHash (Join-Path $GameDir $f)).Hash.Substring(0, 12).ToLowerInvariant()
    Write-Host ("{0} {1}  sha256 {2}" -f $(if ($Restore) { 'restored' } else { 'deployed' }), $f, $h)
}
