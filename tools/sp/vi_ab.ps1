<#
.SYNOPSIS
    View-instanced vs two-pass frames alternating in the same view ("vi ab"), measured by "prepass gpu": the GPU time
    of both eyes' regions, the copies in them, and the playback thread's time and cycles over them.

.EXAMPLE
    tools\sp\vi_ab.ps1 -Seconds 15
#>
param(
    [int] $Seconds = 15,
    [int] $Frames = 30,
    [string] $Ipd = '0.5'
)
$ErrorActionPreference = 'Stop'
$c = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'rdrcmd.ps1'
& $c "double on $Ipd" | Out-Null
& $c 'vi on' | Out-Null
& $c "vi ab $Frames" | Out-Null
& $c 'prepass gpu on' | Out-Null
Start-Sleep -Seconds 3
& $c 'prepass gpu reset' | Out-Null
Start-Sleep -Seconds $Seconds
(& $c 'prepass gpu') -split ' \| ' | Select-Object -Skip 1
& $c 'vi ab 0' | Out-Null
