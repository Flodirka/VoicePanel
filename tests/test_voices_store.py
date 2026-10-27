"""Voice profile store: persistence, bounds, prompt_text index, updates."""
from __future__ import annotations

import json

import pytest

from conftest import make_wav
from voiceservice.voices import MAX_REFERENCE_S, MIN_REFERENCE_S, VoiceProfileError, VoiceStore


def test_create_lists_and_persists(store: VoiceStore, wav_3s: bytes, tmp_path) -> None:
    profile = store.create(name="My Voice", audio_bytes=wav_3s, reference_text="привет", language="ru")

    assert profile["id"] == "my-voice"
    assert profile["reference_text"] == "привет"
    assert profile["reference_duration_s"] == pytest.approx(3.0, abs=0.05)
    assert profile["wav_present"] is True

    # A fresh store over the same directory sees the profile (session restore).
    reopened = VoiceStore(tmp_path / "voice-library")
    assert [p["id"] for p in reopened.list()] == ["my-voice"]

    manifest = json.loads((tmp_path / "voice-library" / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["my-voice"]["language"] == "ru"


def test_prompt_text_matches_audio_cpp_format(store: VoiceStore, wav_3s: bytes) -> None:
    store.create(name="Speaker", audio_bytes=wav_3s, reference_text="hello world", language="en")
    lines = (store.dir / "prompt_text").read_text(encoding="utf-8").splitlines()
    assert lines == ["speaker|hello world"]


def test_reference_must_be_long_enough(store: VoiceStore) -> None:
    with pytest.raises(VoiceProfileError, match="too short"):
        store.create(name="Blip", audio_bytes=make_wav(MIN_REFERENCE_S - 0.5))


def test_reference_must_not_be_too_long(store: VoiceStore) -> None:
    with pytest.raises(VoiceProfileError, match="too long"):
        store.create(name="Long", audio_bytes=make_wav(MAX_REFERENCE_S + 1))


def test_duplicate_name_is_rejected(store: VoiceStore, wav_3s: bytes) -> None:
    store.create(name="Twin", audio_bytes=wav_3s)
    with pytest.raises(VoiceProfileError, match="already exists"):
        store.create(name="Twin", audio_bytes=wav_3s)


def test_update_renames_and_edits_transcript(store: VoiceStore, wav_3s: bytes) -> None:
    created = store.create(name="Old Name", audio_bytes=wav_3s, reference_text="draft")
    updated = store.update(created["id"], name="New Name", reference_text="точный текст")

    assert updated is not None
    assert updated["name"] == "New Name"
    assert updated["reference_text"] == "точный текст"
    assert (store.dir / "prompt_text").read_text(encoding="utf-8").strip() == "old-name|точный текст"


def test_update_rejects_bad_audio(store: VoiceStore, wav_3s: bytes) -> None:
    created = store.create(name="Ref", audio_bytes=wav_3s)
    with pytest.raises(VoiceProfileError, match="outside"):
        store.update(created["id"], audio_bytes=make_wav(0.5))


def test_delete_removes_files_and_index(store: VoiceStore, wav_3s: bytes) -> None:
    created = store.create(name="Temp", audio_bytes=wav_3s, reference_text="bye")

    assert store.delete(created["id"]) is True
    assert store.delete(created["id"]) is False
    assert not (store.dir / "temp.wav").exists()
    assert (store.dir / "prompt_text").read_text(encoding="utf-8") == ""


def test_get_unknown_returns_none(store: VoiceStore) -> None:
    assert store.get("missing") is None
