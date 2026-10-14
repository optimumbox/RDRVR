# Build and run viewtest.cpp (the VS toolchain's cl; the output in the scratch folder below).
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = @(& $vswhere -sort -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)[0]
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$out = Join-Path $env:TEMP 'rdrvr-viewtest'
New-Item -ItemType Directory -Force $out | Out-Null
& cl /nologo /EHsc /O2 /std:c++17 (Join-Path $PSScriptRoot 'viewtest.cpp') "/Fe$out\viewtest.exe" "/Fo$out\\" d3d12.lib dxgi.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
& "$out\viewtest.exe"
