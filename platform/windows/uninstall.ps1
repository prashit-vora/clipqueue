$ErrorActionPreference = 'Stop'
$folder = Join-Path $env:LOCALAPPDATA 'ClipQueue'
$target = Join-Path $folder 'clipqueue.exe'
if (Test-Path $target) { Start-Process -FilePath $target -ArgumentList 'stop' -Wait; Start-Sleep -Milliseconds 300 }
Remove-Item (Join-Path ([Environment]::GetFolderPath('Startup')) 'ClipQueue.lnk') -ErrorAction SilentlyContinue
Remove-Item $folder -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'ClipQueue removed.'
