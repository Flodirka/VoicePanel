# Builds the Qt 6 panel with MSVC + Ninja, then deploys Qt DLLs next to the exe.
param(
    [string]$QtDir = "runtime\Qt\6.8.3\msvc2022_64",
    [string]$BuildDir = "panel\build",
    [switch]$SkipDeploy
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$vcvarsCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
)
$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
    Write-Host "vcvars64.bat not found. Install Visual Studio 2022 (Desktop C++ workload)."
    exit 1
}

$qt = (Resolve-Path $QtDir).Path
$panel = Join-Path $root "panel"
$build = Join-Path $root $BuildDir
$binDir = Join-Path $build "bin"

$bat = @"
@echo off
call "$vcvars" >nul
cmake -S "$panel" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt"
if errorlevel 1 exit /b 1
cmake --build "$build"
"@
$tmp = Join-Path $env:TEMP "vp-build-panel.bat"
Set-Content -Path $tmp -Value $bat -Encoding ASCII
cmd /c $tmp
if ($LASTEXITCODE -ne 0) {
    Write-Host "panel build failed"
    exit 1
}

# Portable layout: the VAD model sits in models/ next to the executable.
$vad = Join-Path $root "runtime\models\silero_vad.onnx"
if (Test-Path $vad) {
    New-Item -ItemType Directory -Force (Join-Path $binDir "models") | Out-Null
    Copy-Item $vad (Join-Path $binDir "models\silero_vad.onnx") -Force
}

if (-not $SkipDeploy) {
    $wdq = Join-Path $qt "bin\windeployqt.exe"
    if (Test-Path $wdq) {
        # windeployqt prints warnings (a missing dxcompiler.dll is normal for a
        # widgets build) on stderr. With $ErrorActionPreference = Stop that
        # becomes a NativeCommandError that aborts the script *after* a
        # successful compile, so the exit code is ignored here on purpose.
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & $wdq --release --no-translations --no-opengl-sw --no-system-d3d-compiler `
            (Join-Path $binDir "VoicePanel.exe") 2>&1 | Out-Null
        $ErrorActionPreference = $previous
    }
    # The test binary links Qt6Test, which windeployqt does not copy because
    # VoicePanel.exe does not link it. Without this the tests die with
    # STATUS_DLL_NOT_FOUND (0xC0000135) and print nothing.
    $qtTest = Join-Path $qt "bin\Qt6Test.dll"
    if (Test-Path $qtTest) {
        Copy-Item $qtTest (Join-Path $binDir "Qt6Test.dll") -Force
    }
}

$ctest = Join-Path $qt "bin\ctest.exe"
if (-not (Test-Path $ctest)) { $ctest = "ctest" }
Write-Host "run the panel tests with: $ctest --test-dir $build --output-on-failure"

Write-Host "panel built: $(Join-Path $binDir 'VoicePanel.exe')"
