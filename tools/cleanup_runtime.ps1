# Removes everything under runtime/ (and the download cache) that the minimal
# VoicePanel setup does not reference, so the app and its models weigh as little
# as possible on disk and in RAM/VRAM. Cloning stays: qwen3-tts-clone and the
# audiocpp-cuda helper are in the keep-list.
#
# Usage:
#   .\tools\cleanup_runtime.ps1 -DryRun     # show what would be removed (default)
#   .\tools\cleanup_runtime.ps1             # actually delete
param([switch]$DryRun)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if ($DryRun) { Write-Host "DRY RUN - nothing is deleted" }

# Keep-list: models the minimal config uses (nemotron ASR + diar, GEC corrector,
# Hy-MT2 translator, kokoro/piper built-in voices, whisper-small file ASR,
# silero VAD, qwen3-tts-clone for voice profiles).
$keepModels = @(
    "nemotron-3.5-asr-streaming-0.6b-q8_0.gguf",
    "nemotron-3-diarization-q8_0.gguf",
    "silero_vad.onnx",
    "Qwen3.5-0.8B-GEC-KAZ-RUS-ENG.Q4_0.gguf",
    "Hy-MT2-1.8B-Q4_K_M.gguf",
    "kokoro-82m-q8_0.gguf",
    "piper-ru-ruslan-medium.gguf",
    "ggml-small.bin",
    "qwen3-tts-12hz-0.6b-base-q8_0.gguf"
)
$keepBins = @("audiocpp-cpu", "audiocpp-cuda", "audiocpp-vulkan", "whisper-cpu", "llama-cuda", "llama-vulkan")

$script:removed = [long]0
$script:freed = [long]0

function Remove-IfNotKept {
    param([string]$Path, [string[]]$Keep)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    if ($Keep -contains (Split-Path -Leaf $Path)) { return }
    $item = Get-Item -LiteralPath $Path
    if ($item.PSIsContainer) {
        $sum = (Get-ChildItem -LiteralPath $Path -Recurse -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
        $size = [long]$sum
    } else {
        $size = [long]$item.Length
    }
    $script:removed = $script:removed + 1
    $script:freed = $script:freed + $size
    $rel = $Path.Substring($root.Length + 1)
    $mb = [math]::Round($size / 1MB, 1)
    Write-Output ("  remove {0}  ({1} MB)" -f $rel, $mb)
    if (-not $DryRun) { Remove-Item -LiteralPath $Path -Recurse -Force }
}

Write-Output "models:"
foreach ($entry in Get-ChildItem -LiteralPath (Join-Path $root "runtime\models") -Force) {
    Remove-IfNotKept -Path $entry.FullName -Keep $keepModels
}
Write-Output "bench models (experiment leftovers):"
Remove-IfNotKept -Path (Join-Path $root "runtime\bench\models") -Keep @()
Write-Output "engine builds:"
foreach ($d in Get-ChildItem -LiteralPath (Join-Path $root "runtime\bin") -Directory) {
    Remove-IfNotKept -Path $d.FullName -Keep $keepBins
}
Write-Output "download cache and test artefacts:"
$extras = @("downloads", "test-records")
foreach ($d in Get-ChildItem -LiteralPath $root -Directory) {
    if ($d.Name -match '^test-state-') { $extras += $d.Name }
}
foreach ($name in $extras) { Remove-IfNotKept -Path (Join-Path $root $name) -Keep @() }

# Saved compute choices can point at removed GPU builds; reset to CPU (clone stays GPU).
if (-not $DryRun) {
    $stateDir = Join-Path $root "runtime\state"
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    '{ "clone": "gpu", "diar": "cpu", "llm": "cpu", "main": "cpu" }' | Set-Content -Path (Join-Path $stateDir "compute.json") -Encoding ASCII
    Write-Output "  reset runtime\state\compute.json to cpu (clone=gpu)"
}
$mbTotal = [math]::Round($script:freed / 1MB, 1)
Write-Output ("done: {0} item(s), {1} MB {2}" -f $script:removed, $mbTotal, $(if ($DryRun) { "would be freed" } else { "freed" }))