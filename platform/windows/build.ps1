$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '../..')
# Use the installed Windows SDK and static C runtime; no redistributable installer.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
$setup = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
cmd /c "`"$setup`" -arch=x64 -host_arch=x64 >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
New-Item -ItemType Directory -Force build | Out-Null
$common = @('/nologo', '/std:c11', '/O1', '/W3', '/MT', '/D_CRT_SECURE_NO_WARNINGS', '/Isrc')
$source = @('src/windows.c', 'src/core.c', 'src/sha256.c')
$libs = @('user32.lib', 'gdi32.lib', 'ole32.lib', 'windowscodecs.lib', 'uuid.lib', 'psapi.lib')
& cl @common @source /Febuild/clipqueue.exe /link /SUBSYSTEM:WINDOWS @libs
if ($LASTEXITCODE) { throw 'Windows build failed' }
& cl @common /DCQ_CONSOLE @source /Febuild/clipqueue-console.exe /link /SUBSYSTEM:CONSOLE @libs
if ($LASTEXITCODE) { throw 'Windows test backend build failed' }
& cl @common tests/portable/core_test.c src/core.c src/sha256.c /Febuild/core-test.exe
if ($LASTEXITCODE) { throw 'Core tests build failed' }
& cl @common tests/windows/integration.c /Febuild/integration-test.exe user32.lib
if ($LASTEXITCODE) { throw 'Integration tests build failed' }
Remove-Item *.obj -ErrorAction SilentlyContinue
