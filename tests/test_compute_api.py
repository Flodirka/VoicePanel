"""Compute settings: which process runs on the CPU or the GPU."""
from __future__ import annotations

import json
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from voiceservice import compute
from voiceservice.app import create_app


def _builds(root: Path, variants: list[tuple[str, str]]) -> None:
    for variant, exe in variants:
        path = root / "runtime" / "bin" / variant / exe
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"")


AUDIOCPP = [("audiocpp-cpu", "audiocpp_server.exe"), ("audiocpp-cuda", "audiocpp_server.exe")]
OMNIVOICE = [("omnivoice-cpu", "omnivoice-tts.exe"), ("omnivoice-cuda", "omnivoice-tts.exe")]
LLAMA = [("llama-vulkan", "llama-server.exe"), ("llama-cuda", "llama-server.exe")]


@pytest.fixture()
def service(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    _builds(tmp_path, AUDIOCPP + LLAMA)
    (tmp_path / "config.yaml").write_text("service:\n  port: 8099\n", encoding="utf-8")
    app = create_app(tmp_path / "config.yaml")
    with TestClient(app, base_url="http://127.0.0.1") as client:
        yield client, tmp_path


def test_snapshot_lists_every_process(service) -> None:
    client, _ = service
    groups = {g["id"]: g for g in client.get("/admin/compute").json()["groups"]}
    # Speaker separation is its own process: it can be placed independently of
    # recognition, which is the point of listing it here.
    assert set(groups) == {"llm", "clone", "main", "diar"}
    assert groups["llm"]["device"] == "cpu"          # gpu_layers 0 by default
    assert groups["clone"]["device"] == "cpu"        # omnivoice.cpp CPU build by default
    assert groups["main"]["device"] == "cpu"
    assert groups["diar"]["device"] == "cpu"
    assert groups["main"]["available"] == ["cpu", "gpu"]
    assert groups["diar"]["available"] == ["cpu", "gpu"]
    assert groups["main"]["process"] == "audiocpp"
    assert groups["clone"]["process"] == "omnivoice"
    assert groups["diar"]["process"] == "audiocpp_diar"
    # Recognition and built-in voices are one process, so one choice covers both.
    assert "one process" in groups["main"]["label"]


def test_diar_process_is_reported_and_switchable(service) -> None:
    client, root = service
    registry = client.app.state.registry
    assert registry.backend_process("diar") is registry.audiocpp_diar.proc
    assert client.get("/admin/status").json()["backends"]["audiocpp_diar"] is not None
    registry.audiocpp_diar.proc._leases = 1
    busy = client.post("/admin/compute", json={"devices": {"diar": "gpu"}})
    assert busy.status_code == 409
    registry.audiocpp_diar.proc._leases = 0
    changed = client.post("/admin/compute", json={"devices": {"diar": "gpu"}})
    assert changed.status_code == 200, changed.text
    assert registry.audiocpp_diar.cfg["backend"] == "cuda"
    assert json.loads(compute.state_path(root).read_text(encoding="utf-8"))["diar"] == "gpu"


def test_saving_the_current_choices_writes_nothing(service) -> None:
    client, root = service
    resp = client.post("/admin/compute", json={"devices": {"main": "cpu", "llm": "cpu"}})
    assert resp.status_code == 200, resp.text
    assert not compute.state_path(root).exists()


def test_a_busy_process_blocks_the_change(service) -> None:
    client, root = service
    registry = client.app.state.registry
    registry.audiocpp.proc._leases = 1               # a request holds the process
    resp = client.post("/admin/compute", json={"devices": {"main": "gpu"}})
    assert resp.status_code == 409
    assert "busy" in resp.json()["detail"]
    assert registry.cfg["backends"]["audiocpp"]["backend"] == "cpu"
    assert not compute.state_path(root).exists()
    registry.audiocpp.proc._leases = 0


def test_an_idle_process_is_switched_and_the_choice_persists(service) -> None:
    client, root = service
    resp = client.post("/admin/compute", json={"devices": {"main": "gpu", "llm": "gpu"}})
    assert resp.status_code == 200, resp.text
    devices = {g["id"]: g["device"] for g in resp.json()["groups"]}
    assert devices["main"] == "gpu" and devices["llm"] == "gpu"

    registry = client.app.state.registry
    assert registry.cfg["backends"]["audiocpp"]["backend"] == "cuda"
    assert Path(registry.cfg["backends"]["audiocpp"]["binary"]).parent.name == "audiocpp-cuda"
    assert registry.cfg["backends"]["llama"]["gpu_layers"] == 99
    assert registry.audiocpp.cfg["backend"] == "cuda"      # rebuilt, not just edited

    saved = json.loads(compute.state_path(root).read_text(encoding="utf-8"))
    assert saved == {"llm": "gpu", "main": "gpu"}


def test_saved_choices_are_applied_at_startup(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    _builds(tmp_path, AUDIOCPP + LLAMA)
    (tmp_path / "config.yaml").write_text("service:\n  port: 8098\n", encoding="utf-8")
    state = compute.state_path(tmp_path)
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(json.dumps({"main": "gpu", "clone": "cpu"}), encoding="utf-8")

    app = create_app(tmp_path / "config.yaml")
    with TestClient(app, base_url="http://127.0.0.1"):
        backends = app.state.cfg["backends"]
        assert backends["audiocpp"]["backend"] == "cuda"
        assert backends["omnivoice"]["backend"] == "cpu"
        assert backends["llama"]["gpu_layers"] == 0
    # config.yaml itself stays untouched: choices live in the state file.
    assert "backend" not in (tmp_path / "config.yaml").read_text(encoding="utf-8")


def test_gpu_is_not_offered_without_a_cuda_build(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    _builds(tmp_path, [("audiocpp-cpu", "audiocpp_server.exe"), ("llama-vulkan", "llama-server.exe")])
    (tmp_path / "config.yaml").write_text("service:\n  port: 8097\n", encoding="utf-8")
    app = create_app(tmp_path / "config.yaml")
    with TestClient(app, base_url="http://127.0.0.1") as client:
        groups = {g["id"]: g for g in client.get("/admin/compute").json()["groups"]}
        assert groups["main"]["available"] == ["cpu"]
        assert groups["llm"]["available"] == ["cpu"]
        resp = client.post("/admin/compute", json={"devices": {"main": "gpu"}})
        assert resp.status_code == 400
        assert "missing" in resp.json()["detail"]
