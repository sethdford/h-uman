"""Tests for pro_clone.py pure helpers. Run: python3 -m pytest scripts/voice/test_pro_clone.py"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pro_clone as pc  # noqa: E402

AFINFO = """File:           take1.m4a
Data format:     1 ch,  44100 Hz, aac  (0x00000000) 0 bits/channel, 0 bytes/packet
estimated duration: 612.345 sec
audio bytes: 123
"""


def test_parse_afinfo_reads_duration_channels_rate():
    info = pc.parse_afinfo(AFINFO)
    assert info == {"duration_sec": 612.345, "channels": 1, "sample_rate": 44100}
    assert pc.parse_afinfo("garbage") == {"duration_sec": None, "channels": None,
                                          "sample_rate": None}


def test_plan_sums_minutes_and_applies_threshold():
    items = [{"duration_sec": 600.0}, {"duration_sec": 900.0}, {"duration_sec": None}]
    p = pc.plan(items)
    assert p["files"] == 3 and p["minutes"] == 25.0 and p["enough"] is False
    p = pc.plan(items + [{"duration_sec": 300.0}])
    assert p["minutes"] == 30.0 and p["enough"] is True


def test_collect_inputs_walks_dirs_and_filters_extensions(tmp_path):
    (tmp_path / "b.wav").write_bytes(b"x")
    (tmp_path / "a.m4a").write_bytes(b"x")
    (tmp_path / "notes.txt").write_bytes(b"x")
    files = pc.collect_inputs([str(tmp_path), str(tmp_path / "notes.txt"), str(tmp_path / "b.wav")])
    assert [os.path.basename(f) for f in files] == ["a.m4a", "b.wav", "b.wav"]


def test_multipart_has_fields_file_and_boundary():
    body, ctype = pc.multipart({"purpose": "fine_tune"}, "file", "001.wav", b"RIFFdata")
    boundary = ctype.split("boundary=")[1]
    assert body.startswith(f"--{boundary}".encode())
    assert b'name="purpose"\r\n\r\nfine_tune' in body
    assert b'name="file"; filename="001.wav"' in body and b"Content-Type: audio/wav" in body
    assert b"RIFFdata" in body and body.endswith(f"\r\n--{boundary}--\r\n".encode())


def test_fine_tune_body_matches_documented_fields():
    b = pc.fine_tune_body("Seth Pro", "ds_1", "sonic-3.6", "en", "desc")
    assert b == {"name": "Seth Pro", "description": "desc", "language": "en",
                 "model_id": "sonic-3.6", "dataset": "ds_1"}


def test_adopt_persona_swaps_id_keeps_old_and_everything_else():
    persona = {"core": {"identity": "x"},
               "voice": {"provider": "cartesia", "voice_id": "old", "model": "sonic-3.6",
                         "default_speed": 0.85},
               "voice_messages": {"enabled": True, "frequency": "rare"}}
    out = pc.adopt_persona(persona, "new")
    assert out["voice"]["voice_id"] == "new" and out["voice"]["previous_voice_id"] == "old"
    assert out["voice"]["model"] == "sonic-3.6" and out["voice"]["default_speed"] == 0.85
    assert out["voice_messages"] == persona["voice_messages"] and out["core"] == persona["core"]
    assert persona["voice"]["voice_id"] == "old"  # input untouched
    # no prior voice block: one is created with the provider set
    out = pc.adopt_persona({"core": {}}, "new")
    assert out["voice"] == {"voice_id": "new", "provider": "cartesia"}
    # same id twice does not clobber previous_voice_id
    out = pc.adopt_persona(out, "new")
    assert "previous_voice_id" not in out["voice"]
