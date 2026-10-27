# Starts the VoicePanel desktop panel (Qt build), if present.
param(
    [string]$Exe = "panel\build\bin\VoicePanel.exe"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$path = Join-Path $root $Exe
if (-not (Test-Path $path)) {
    Write-Host "panel executable not found: $path"
    Write-Host "build it first: tools\build_panel.ps1"
    exit 1
}

$running = @(Get-CimInstance Win32_Process -Filter "Name='VoicePanel.exe'" | Where-Object { $_.CommandLine -notmatch '--service-tray' })
if ($running) {
    Write-Host "VoicePanel is already running (pid $($running[0].ProcessId))"
    exit 0
}

Start-Process -FilePath $path -WorkingDirectory (Split-Path -Parent $path) | Out-Null
Write-Host "VoicePanel started from $path"
