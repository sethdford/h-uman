#!/usr/bin/env python3
"""Tests for harvest_imessage_voice.py — the no-inbound voice-corpus harvest."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import harvest_imessage_voice as h


def test_is_voice_signal_keeps_prose():
    assert h.is_voice_signal("yeah for a bit, gonna grab food soon")
    assert h.is_voice_signal("haha fair, lemme know")


def test_is_voice_signal_drops_url_only():
    assert not h.is_voice_signal("https://www.tiktok.com/t/ZTkGsX")
    assert not h.is_voice_signal("https://maps.app.goo.gl/zpNcYr")


def test_is_voice_signal_keeps_prose_with_url():
    # prose around a link is still voice
    assert h.is_voice_signal("check this out https://example.com it's wild")


def test_is_voice_signal_drops_tapback_echoes():
    assert not h.is_voice_signal("Liked “see you then”")
    assert not h.is_voice_signal("Loved an image")
    assert not h.is_voice_signal("Emphasized “ok”")


def test_is_voice_signal_drops_pure_emoji_punct():
    assert not h.is_voice_signal("😂😂😂")
    assert not h.is_voice_signal("!!!")


def test_decode_attributed_body_short_length():
    # streamtyped shape: ...NSString...+<len-byte><utf8>
    blob = b"\x04\x0bstreamtyped\x81NSString\x01\x94\x84+" + bytes([5]) + b"hello"
    assert h.decode_attributed_body(blob) == "hello"


def test_decode_attributed_body_long_length():
    text = b"x" * 200
    blob = b"NSString+\x81" + (200).to_bytes(2, "little") + text
    assert h.decode_attributed_body(blob) == "x" * 200


def test_decode_attributed_body_empty_and_plain():
    assert h.decode_attributed_body(b"") == ""
    assert h.decode_attributed_body(None) == ""
    assert h.decode_attributed_body("already a string") == "already a string"


def main():
    tests = [
        test_is_voice_signal_keeps_prose,
        test_is_voice_signal_drops_url_only,
        test_is_voice_signal_keeps_prose_with_url,
        test_is_voice_signal_drops_tapback_echoes,
        test_is_voice_signal_drops_pure_emoji_punct,
        test_decode_attributed_body_short_length,
        test_decode_attributed_body_long_length,
        test_decode_attributed_body_empty_and_plain,
    ]
    print("Testing harvest_imessage_voice.py")
    print("=" * 60)
    p = f = 0
    for t in tests:
        try:
            t()
            print(f"✓ {t.__name__}")
            p += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            f += 1
        except Exception as e:  # noqa: BLE001
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}")
            f += 1
    print("=" * 60)
    print(f"Results: {p} passed, {f} failed")
    return 0 if f == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
