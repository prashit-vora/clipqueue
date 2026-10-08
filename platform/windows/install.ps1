$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'clipqueue.exe'
if (!(Test-Path $source)) { $source = Join-Path $PSScriptRoot '../../build/clipqueue.exe' }
if (!(Test-Path $source)) { throw 'Build ClipQueue first, or extract the complete Windows download.' }
$folder = Join-Path $env:LOCALAPPDATA 'ClipQueue'
$target = Join-Path $folder 'clipqueue.exe'
New-Item -ItemType Directory -Force $folder | Out-Null
if (Test-Path $target) {
    Start-Process -FilePath $target -ArgumentList 'stop' -Wait
    Start-Sleep -Milliseconds 300
}
Copy-Item $source $target -Force
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Startup')) 'ClipQueue.lnk'))
$link.TargetPath = $target
$link.Arguments = '--daemon'
$link.Description = 'ClipQueue background clipboard queue'
$link.Save()
Start-Process -FilePath $target -ArgumentList 'start'
Write-Host 'Installed. Ctrl+Alt+Q toggles queue mode; Ctrl+Alt+Backspace clears it. Ctrl+V pastes the next item.'
Write-Host "Controls: & '$target' status (or on, off, clear, stop)"
