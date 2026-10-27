"""Negative checks for the public-file verifier; payloads are synthetic."""
import json

from tools.verify_publication import scan_files


def test_credentials_are_reported_without_disclosing_the_value(tmp_path):
    value = "ghp_" + "A" * 36
    (tmp_path / "sample.txt").write_text(value)
    findings = scan_files(tmp_path, ["sample.txt"])
    assert any(item["rule"] == "credential-format" for item in findings)
    assert value not in json.dumps(findings)


def test_runtime_and_recordings_cannot_be_published(tmp_path):
    (tmp_path / "runtime").mkdir()
    (tmp_path / "runtime/reference.wav").write_bytes(b"example")
    findings = scan_files(tmp_path, ["runtime/reference.wav"])
    assert any(item["rule"] == "private-artifact" for item in findings)


def test_svg_cannot_load_active_content(tmp_path):
    (tmp_path / "banner.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg"><script>example</script></svg>')
    findings = scan_files(tmp_path, ["banner.svg"])
    assert any(item["rule"] == "active-svg" for item in findings)


def test_untrusted_workflows_cannot_get_write_permissions(tmp_path):
    path = tmp_path / ".github/workflows"
    path.mkdir(parents=True)
    (path / "bad.yml").write_text("on: pull_request_target\npermissions:\n  contents: write\n")
    findings = scan_files(tmp_path, [".github/workflows/bad.yml"])
    assert any(item["rule"] == "privileged-workflow" for item in findings)
