import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import insight_stream as ins  # noqa: E402


def test_prompt_keeps_proper_nouns_capitalized_and_asks_for_names():
    system, _ = ins.build_prompt("id", "Sam", "friend", ["[t0] them: hi"], 8)
    assert "Lowercase, like a note to yourself" not in system
    assert "capitalized" in system and '"names"' in system


def test_parse_notes_returns_names_and_raw_tokens():
    raw = ('[{"note":"Priya surgery tuesday","kind":"plan","confidence":0.9,'
           '"evidence":["t0","d1"],"names":[{"name":"Priya","type":"person"},'
           '{"name":"x","type":"weird"},{"name":"","type":"place"}]}]')
    n = ins.parse_notes(raw, 8)[0]
    assert n["evidence_tokens"] == ["t0", "d1"]
    assert n["names"] == [{"name": "Priya", "type": "person"}]
    assert n["evidence"] == [0, 1]  # legacy int view unchanged
