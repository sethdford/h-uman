import io
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import insight_stream as ins  # noqa: E402

# origin/main's persona-pass system prompt for the same inputs, byte for byte.
# The persona pass is LIVE and unmeasured against any other wording, so this
# branch must not change it (R12 I1); only the wide pass gets the new prompt.
MAIN_PERSONA_SYSTEM = (
    'You are Seth Ford. id\n\nYou just reread your recent texts with Sam (friend) and are '
    'jotting private notes to yourself — the things YOU would actually remember and bring up '
    'next time: specific names, places, plans with when, what they\'re dealing with, running '
    'jokes and inside references, what they like and don\'t. Never generic traits ("is '
    'friendly"), never advice, never anything not in the texts. Lowercase, like a note to '
    'yourself, present tense, each under 110 characters.\n\nOutput ONLY a JSON array of at most '
    '8 objects: {"note": str, "kind": "fact"|"thread"|"plan"|"preference"|"inside_ref", '
    '"confidence": number 0-1, "evidence": [the [tN] numbers of the texts the note comes '
    'from]}. No prose before or after.'
)


def test_persona_prompt_is_byte_identical_to_origin_main():
    system, user = ins.build_prompt("id", "Sam", "friend", ["[t0] them: hi"], 8)
    assert system == MAIN_PERSONA_SYSTEM
    assert user == "recent texts (oldest first):\n[t0] them: hi"


def test_wide_prompt_keeps_proper_nouns_capitalized_and_asks_for_names():
    system, _ = ins.build_prompt("id", "Sam", "friend", ["[t0] them: hi"], 8, wide=True)
    assert "Lowercase, like a note to yourself" not in system
    assert "capitalized" in system and '"names"' in system and "[dN]" in system


def test_call_model_max_tokens_defaults_to_700_and_is_overridable(monkeypatch):
    bodies = []

    def fake_urlopen(req, timeout=None):
        bodies.append(json.loads(req.data))
        return io.BytesIO(json.dumps({"choices": [{"message": {"content": " ok "}}]}).encode())
    monkeypatch.setattr(ins.urllib.request, "urlopen", fake_urlopen)
    assert ins.call_model("http://127.0.0.1:1/v1/x", "m", "s", "u") == "ok"
    ins.call_model("http://127.0.0.1:1/v1/x", "m", "s", "u", max_tokens=1500)
    assert [b["max_tokens"] for b in bodies] == [700, 1500]


def test_parse_notes_returns_names_and_raw_tokens():
    raw = ('[{"note":"Priya surgery tuesday","kind":"plan","confidence":0.9,'
           '"evidence":["t0","d1"],"names":[{"name":"Priya","type":"person"},'
           '{"name":"x","type":"weird"},{"name":"","type":"place"}]}]')
    n = ins.parse_notes(raw, 8)[0]
    assert n["evidence_tokens"] == ["t0", "d1"]
    assert n["names"] == [{"name": "Priya", "type": "person"}]
    assert n["evidence"] == [0, 1]  # legacy int view unchanged
