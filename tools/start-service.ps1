# Starts the Voice AI Service in the background and waits until it is healthy.
param(
    [string]$Config = "config.yaml",
    [int]$Port = 8080
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path $Config)) {
    Write-Host "config not found: $Config (copy config.example.yaml to config.yaml first)"
    exit 1
}

function Start-ServiceIndicator {
    $indicator = Join-Path $root 'panel\build\bin\VoicePanel.exe'
    if (Test-Path -LiteralPath $indicator) {
        Start-Process -FilePath $indicator -ArgumentList @('--service-tray', "http://127.0.0.1:$Port") -WorkingDirectory (Split-Path -Parent $indicator) -WindowStyle Hidden | Out-Null
    } else {
        Write-Host 'Service tray indicator is unavailable until tools\build_panel.ps1 is built.'
    }
}

& python -c "import fastapi, uvicorn" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "the 'python' on PATH has no service dependencies. Install them with:"
    Write-Host "  python -m pip install --user -r service\requirements.txt"
    exit 1
}

$listening = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if ($listening) {
    Write-Host "something is already listening on port $Port (pid $($listening.OwningProcess)); reusing it"
    Start-ServiceIndicator
    exit 0
}

$logDir = Join-Path $root "runtime\logs"
New-Item -ItemType Directory -Force $logDir | Out-Null
$outLog = Join-Path $logDir "service.log"
$errLog = Join-Path $logDir "service.err.log"

$proc = Start-Process -FilePath "python" `
    -ArgumentList @("tools\run_service.py", "--config", $Config) `
    -WorkingDirectory $root -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog

$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 700
    try {
        $health = Invoke-RestMethod "http://127.0.0.1:$Port/health" -TimeoutSec 2
        if ($health.status -eq "ok") {
            Write-Host "service is up (pid $($proc.Id), v$($health.version)) on http://127.0.0.1:$Port"
            Write-Host "logs: $outLog"
            Start-ServiceIndicator
            exit 0
        }
    } catch { }
}

Write-Host "service did not become healthy; tail of ${errLog}:"
Get-Content $errLog -Tail 20 -ErrorAction SilentlyContinue
exit 1
