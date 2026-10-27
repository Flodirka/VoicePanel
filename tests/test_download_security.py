"""Download integrity and extraction boundaries, without network access."""
import hashlib
import io
import zipfile

import pytest

from tools.download_runtime import download_file, extract_archive, verify_file


def test_cached_file_requires_the_locked_hash(tmp_path):
    archive = tmp_path / "engine.zip"
    archive.write_bytes(b"modified")
    with pytest.raises(ValueError, match="SHA-256"):
        verify_file(archive, hashlib.sha256(b"expected").hexdigest())


def test_non_https_download_is_rejected_before_network_or_writes(tmp_path):
    with pytest.raises(ValueError, match="HTTPS"):
        download_file("http://example.invalid/engine.zip", tmp_path / "engine.zip", "0" * 64)
    assert not (tmp_path / "engine.zip").exists()


def test_hash_failure_never_installs_the_download(tmp_path, monkeypatch):
    monkeypatch.setattr("tools.download_runtime.open_url", lambda url: io.BytesIO(b"modified"))
    target = tmp_path / "engine.zip"
    with pytest.raises(ValueError, match="SHA-256"):
        download_file("https://github.com/example/release.zip", target, "0" * 64)
    assert not target.exists()


@pytest.mark.parametrize("name", ["../escape.txt", "/escape.txt", "C:/escape.txt", "..\\escape.txt"])
def test_zip_paths_cannot_escape_the_destination(tmp_path, name):
    archive = tmp_path / "engine.zip"
    with zipfile.ZipFile(archive, "w") as output:
        output.writestr(name, b"untrusted")
    with pytest.raises(ValueError, match="archive"):
        extract_archive(archive, tmp_path / "engine")
    assert not (tmp_path / "escape.txt").exists()


def test_archive_symlinks_are_rejected(tmp_path):
    archive = tmp_path / "engine.zip"
    link = zipfile.ZipInfo("link")
    link.create_system = 3
    link.external_attr = 0o120777 << 16
    with zipfile.ZipFile(archive, "w") as output:
        output.writestr(link, "../outside")
    with pytest.raises(ValueError, match="symlink"):
        extract_archive(archive, tmp_path / "engine")


def test_valid_archive_is_extracted_without_execution(tmp_path):
    archive = tmp_path / "engine.zip"
    with zipfile.ZipFile(archive, "w") as output:
        output.writestr("package/data.txt", "example")
    extract_archive(archive, tmp_path / "engine", strip_prefix="package/")
    assert (tmp_path / "engine/data.txt").read_text() == "example"
