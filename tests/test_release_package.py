import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
POWERSHELL = shutil.which("pwsh") or shutil.which("powershell")
pytestmark = pytest.mark.skipif(os.name != "nt" or not POWERSHELL, reason="Windows package scripts")


def test_package_only_copies_public_source_and_deployed_runtime(tmp_path: Path):
    files = {
        "service/voiceservice/__init__.py": '__version__ = "0.2.0"\n',
        "config.example.yaml": "service:\n  host: 127.0.0.1\nbackends:\n  llama:\n    binary: runtime/bin/llama-vulkan/llama-server.exe\n",
        "README.md": "# Release\n",
        "docs/public.md": "Public documentation.\n",
        "assets/banner.svg": '<svg xmlns="http://www.w3.org/2000/svg"/>',
        ".gitignore": "/runtime/\n/panel/build/\n/config.yaml\n/docs/private.md\n/tools/private.ps1\n",
    }
    for name, text in files.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    script = tmp_path / "tools/package_release.ps1"
    script.parent.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / "tools/package_release.ps1", script)
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "add", "."], check=True)
    for name in ("docs/private.md", "tools/private.ps1", "config.yaml",
                 "panel/build/bin/VoicePanel.exe", "panel/build/bin/check-bundle.txt", "panel/build/bin/Qt6Test.dll"):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("local-only", encoding="utf-8")
    result = subprocess.run([POWERSHELL, "-NoProfile", "-File", str(script), "-CpuOnly", "-NoModels", "-Zip"],
                            capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, result.stderr
    bundle = tmp_path / "dist/VoicePanel"
    assert (bundle / "assets/banner.svg").is_file()
    for name in ("docs/private.md", "tools/private.ps1", "check-bundle.txt", "Qt6Test.dll"):
        assert not (bundle / name).exists(), name
    expected = files["config.example.yaml"].replace("llama-vulkan", "llama-cpu")
    assert (bundle / "config.yaml").read_text(encoding="utf-8-sig") == expected
    assert (tmp_path / "dist/VoicePanel-0.2.0.zip").is_file()
    assert (tmp_path / "dist/VoicePanel-0.2.0.zip.sha256").is_file()


def test_panel_launcher_finds_the_executable_at_the_package_root(tmp_path: Path):
    tools = tmp_path / "tools"
    tools.mkdir()
    shutil.copyfile(ROOT / "tools/start-panel.ps1", tools / "start-panel.ps1")
    (tmp_path / "VoicePanel.exe").write_text("test fixture", encoding="utf-8")
    quote = lambda value: "'" + str(value).replace("'", "''") + "'"
    command = (
        "function global:Get-CimInstance {}\n"
        "function global:Start-Process { param($FilePath, $WorkingDirectory) $global:started = $FilePath }\n"
        f"& {quote(tools / 'start-panel.ps1')}\n"
        f"if ($global:started -ne {quote(tmp_path / 'VoicePanel.exe')}) {{ exit 2 }}\n"
    )
    result = subprocess.run([POWERSHELL, "-NoProfile", "-Command", command], capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
