# Assembles a distributable bundle at dist\VoicePanel (optionally zipped with a
# SHA-256). Modeled on WinVosk's package_release.py: one folder that runs
# without an installer, with licenses travelling alongside.
#
# Usage:
#   .\tools\package_release.ps1                     # full bundle (copies runtime)
#   .\tools\package_release.ps1 -CpuOnly            # skip CUDA/Vulkan builds
#   .\tools\package_release.ps1 -Config config.yaml # ship a specific config.yaml (local testing)
#   .\tools\package_release.ps1 -Zip                # also write dist\VoicePanel-<ver>.zip + .sha256
param(
    [string]$Out = "dist",
    [string]$PanelBin = "panel\build\bin",
    [string]$RuntimeBin = "runtime\bin",
    [string]$Config = "",
    [string[]]$Models = @(
        "nemotron-3.5-asr-streaming-0.6b-q8_0.gguf",
        "nemotron-3-diarization-q8_0.gguf",
        "silero_vad.onnx",
        "Qwen3.5-0.8B-GEC-KAZ-RUS-ENG.Q4_0.gguf",
        "kokoro-82m-q8_0.gguf",
        "ggml-small.bin",
        "piper-ru-ruslan-medium.gguf",
        "piper-ru-irina-medium.gguf"
    ),
    [string[]]$ModelDirs = @("plamo-3-translate", "omnivoice"),
    [switch]$NoModels,
    [switch]$CpuOnly,
    [switch]$Zip
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$version = (Select-String -Path "service\voiceservice\__init__.py" -Pattern '__version__' | Select-Object -First 1).Line -replace '.*"([^"]+)".*', '$1'
$bundle = [IO.Path]::GetFullPath((Join-Path $root (Join-Path $Out "VoicePanel")))
if (-not $bundle.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Release output must stay inside the checkout: $bundle"
}
if (Test-Path $bundle) { Remove-Item -LiteralPath $bundle -Recurse -Force }
New-Item -ItemType Directory -Force -Path $bundle | Out-Null

function Copy-Panel {
    if (-not (Test-Path (Join-Path $PanelBin "VoicePanel.exe"))) {
        throw "panel not built: $PanelBin\VoicePanel.exe (run tools\build_panel.ps1 first)"
    }
    # The windeployqt output, minus screenshots, the test binary and test artefacts.
    Get-ChildItem $PanelBin -Recurse -File | Where-Object {
        $_.FullName -notmatch 'test-records|test-state-' -and
        $_.Extension -ne '.png' -and
        $_.Name -notin @('VoicePanelTests.exe', 'Qt6Test.dll', 'check-bundle.txt')
    } | ForEach-Object {
        $rel = $_.FullName.Substring((Resolve-Path $PanelBin).Path.Length).TrimStart('\')
        $dest = Join-Path $bundle $rel
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
    }
    Write-Host "panel: copied from $PanelBin"
}

function Copy-Code {
    $publicFiles = @(& git -c core.quotepath=false ls-files -- service tools docs licenses assets panel/resources/fonts/OFL.txt)
    if ($LASTEXITCODE -ne 0) { throw 'Packaging requires the source Git checkout' }
    foreach ($relative in $publicFiles) {
        $target = Join-Path $bundle $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $target -Force
    }
    foreach ($f in @("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md", "config.example.yaml", "ROADMAP.md", "CHANGELOG.md")) {
        if (Test-Path $f) { Copy-Item -LiteralPath $f -Destination (Join-Path $bundle $f) -Force }
    }
    # The bundle ships a ready config.yaml: the minimal base by default
    # (config.example.yaml), or -Config <file> for a custom one.
    $shipConfig = if ($Config -and (Test-Path $Config)) { $Config } else { "config.example.yaml" }
    Copy-Item -LiteralPath $shipConfig -Destination (Join-Path $bundle "config.yaml") -Force
    if ($CpuOnly) {
        $configPath = Join-Path $bundle "config.yaml"
        (Get-Content -LiteralPath $configPath -Raw).Replace('runtime/bin/llama-vulkan/llama-server.exe', 'runtime/bin/llama-cpu/llama-server.exe') |
            Set-Content -LiteralPath $configPath -Encoding utf8 -NoNewline
    }
    Write-Host "config: shipped $shipConfig as config.yaml"
    # The panel looks for silero next to its exe in the portable layout.
    $portableModels = Join-Path $bundle "models"
    New-Item -ItemType Directory -Force -Path $portableModels | Out-Null
    $silero = Join-Path $root "runtime\models\silero_vad.onnx"
    if (Test-Path $silero) { Copy-Item -LiteralPath $silero -Destination (Join-Path $portableModels "silero_vad.onnx") -Force }
    Write-Host "code: service/, tools/, docs/, licenses/ copied"
}

function Copy-Runtime {
    # All engine builds ship: CPU always works; audiocpp-cuda/llama-cuda power the
    # clone helper and the GPU option in Server settings (compute groups).
    $binDirs = @("audiocpp-cpu", "whisper-cpu", "llama-cpu", "omnivoice-cpu")
    if (-not $CpuOnly) { $binDirs += @("audiocpp-cuda", "audiocpp-vulkan", "llama-cuda", "llama-vulkan", "omnivoice-cuda") }
    foreach ($d in $binDirs) {
        $src = Join-Path $RuntimeBin $d
        if (Test-Path $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $bundle "runtime\bin\$d") -Recurse -Force }
    }
    $modelFiles = $Models
    if ($NoModels) { $modelFiles = @() }
    New-Item -ItemType Directory -Force -Path (Join-Path $bundle "runtime\models") | Out-Null
    foreach ($m in $modelFiles) {
        $src = Join-Path $root "runtime\models\$m"
        if (Test-Path $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $bundle "runtime\models\$m") -Force
        } else {
            Write-Host "  warn: model not found, skipped: $m"
        }
    }
    foreach ($d in $ModelDirs) {
        if ($NoModels) { continue }
        $src = Join-Path $root "runtime\models\$d"
        if (Test-Path $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $bundle "runtime\models\$d") -Recurse -Force }
    }
    $espeak = Join-Path $root "runtime\third_party\espeak-ng"
    if (Test-Path $espeak) {
        New-Item -ItemType Directory -Force -Path (Join-Path $bundle "runtime\third_party") | Out-Null
        Copy-Item -LiteralPath $espeak -Destination (Join-Path $bundle "runtime\third_party\espeak-ng") -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $bundle "runtime\voice-library") | Out-Null
    Write-Host "runtime: copied (CpuOnly=$CpuOnly)"
}

Copy-Panel
Copy-Code
Copy-Runtime

$size = (Get-ChildItem $bundle -Recurse -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
Write-Host ("bundle ready: " + $bundle + "  (" + [math]::Round($size / 1MB, 1) + " MB)")

if ($Zip) {
    $zip = Join-Path $root (Join-Path $Out "VoicePanel-$version.zip")
    if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path $bundle -DestinationPath $zip -CompressionLevel Optimal
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
    "$hash  $($zip | Split-Path -Leaf)" | Set-Content -Path "$zip.sha256" -Encoding ASCII
    Write-Host "zip: $zip"
    Write-Host "sha256: $hash  (written to $zip.sha256)"
}
exit 0
