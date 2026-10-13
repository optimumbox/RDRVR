<#
.SYNOPSIS
    Send one command to a running RDRVR through its test channel and print the result.

.DESCRIPTION
    test_channel.cpp: the mod runs each new "<seq> <command>" written to RDRVR_cmd.txt once and reports
    cmd_seq/cmd_result in RDRVR_status.json (rewritten every 250 ms). This writes the next seq and waits for it.

.EXAMPLE
    tools\sp\rdrcmd.ps1 perf
    tools\sp\rdrcmd.ps1 "double on 0.064"
    tools\sp\rdrcmd.ps1 -Status    # the whole status file
#>
param(
    [Parameter(Position = 0)] [string] $Command,
    [string] $GameDir = 'G:\Games\Red Dead Redemption',
    [int] $TimeoutSec = 20,
    [switch] $Status
)
$ErrorActionPreference = 'Stop'
$statusPath = Join-Path $GameDir 'RDRVR_status.json'
$cmdPath = Join-Path $GameDir 'RDRVR_cmd.txt'

function Read-Status {
    for ($i = 0; $i -lt 20; $i++) {
        try { return Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json } catch { Start-Sleep -Milliseconds 100 }
    }
    throw "cannot read $statusPath (is the game running with the mod?)"
}

$s = Read-Status
if ($Status -or -not $Command) { $s; return }
$seq = [long]$s.cmd_seq + 1
[IO.File]::WriteAllText($cmdPath, "$seq $Command`n")
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 150
    $s = Read-Status
    if ([long]$s.cmd_seq -ge $seq) { $s.cmd_result; return }
}
throw "no result for '$Command' (seq $seq) within $TimeoutSec s"
