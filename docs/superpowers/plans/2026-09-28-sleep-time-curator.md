---
title: Sleep-time curator — implementation plan (approach A)
date: 2026-09-28
status: draft (awaiting review)
---

# Sleep-time curator (approach A) — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task by task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Widen the nightly `scripts/insight_stream.py` curator from ~15 persona
contacts to every 1:1 contact Seth texts. It reads chat.db, cites only human
messages as evidence, writes named and verified notes into `contact_insights`
with `source='curator_wide:…'`, and gates rendering of those rows behind
`HU_INSIGHT_WIDE` (default off).

**Architecture:**
- Two small, pure Python modules: `curator_population.py` (who) and
  `curator_evidence.py` (turns, citations, name check). Both are unit-tested
  without chat.db or a model.
- `insight_stream.py` gains `--population wide`, which wires those modules into
  the existing generate → 3-vote verify → admit → write loop.
- One C change: `hu_contact_insights_render` filters `curator_wide` rows by the
  `HU_INSIGHT_WIDE` gate.
- Step C (mirroring names into graph.db) is a separate, later plan.

**Tech stack:** Python 3 stdlib + pytest (tests in `tests/`), SQLite, C11
(`src/memory/repos/`), local GLM on `127.0.0.1:8741`.

**Spec:** `docs/superpowers/specs/2026-09-27-sleep-time-curator-design.md`

## Global constraints

- Extraction talks **only** to a local model. `insight_stream.main` already
  refuses non-loopback `--url`; keep that.
- chat.db is opened **read-only** (`file:…?mode=ro`, via `eval_conversation_quality`).
- Eligibility: 1:1 only; in the last **30 days**, **≥ 10** messages from the contact
  **and ≥ 5** from Seth; `+E.164` or email; **not** a short code.
- Hard exclusions: memory.db `contact_suppressions`, and `~/.human/curator_never.json`
  (missing file = empty list; malformed file = refuse the run).
- Only the contact's messages and Seth's own messages (attribution label `seth`)
  are citable (`[tN]`). Daemon-authored **and ambiguous** sends are shown as `[dN]`
  and are never citable.
- Every name in `names` must appear verbatim, word-bounded, case-insensitively, in
  the text of the note's cited messages. Otherwise the **whole note** is dropped
  and counted.
- Stored source: `curator_wide:` + today's `source_tag` (e.g.
  `curator_wide:extractor:v2:k3:a3`).
- Render gate: `HU_INSIGHT_WIDE` = `off` (default) | `shadow` | `live`, parsed by
  `hu_gate_mode_from_env`.
- The manifest holds **counts only**, never message or note text.
- Refuse (non-zero exit, write nothing) if chat.db is unreadable, the model server
  is down, the never-file is malformed, or 0 contacts are eligible.
- **Plan decision (narrows spec §3 wording):** the wide pass skips persona contacts,
  because the existing persona pass (memory.db turns) already curates them nightly.
  Coverage is still a superset, and no contact is curated twice.

## Review focus

1. **One person under two handles** (phone and email). Each handle is curated and
   filed separately, because the daemon's `memory_session_id` is the handle.
   Test: an email handle with enough traffic is eligible.
2. **Possessives and punctuation around names.** "Priya's surgery" must satisfy
   `names=[Priya]`, "Al" must **not** match inside "Also", and a name split across
   an emoji must still match. Tests in Task 3.
3. **Opt-out after insights already exist.** A contact who opts out tonight must
   have their live rows retired, whatever the source (persona or wide). Test in
   Task 6.
4. **Deadline reached mid-contact.** No partial write for a contact; the contact is
   first in line next night. Test in Task 6.
5. **Attachment-only or emoji-only turns.** They render as `[attachment]` and never
   satisfy a name check. Test in Task 2.

---

### Task 1: Population — who gets curated

**Files:**
- Create: `scripts/curator_population.py`
- Test: `tests/test_curator_population.py`

**Interfaces:**
- Produces:
  - `is_short_code(handle: str) -> bool`
  - `eligible_handles(timelines: dict[str, list[dict]], persona_ids: set[str], now: datetime, window_days=30, min_them=10, min_me=5) -> list[str]`
  - `load_suppressed(mem_db_path: str) -> set[str]`
  - `load_never(path: str) -> set[str]`, raising `ValueError` when the file is malformed
  - `exclusion_reason(handle, suppressed, never) -> str | None`
- Timeline items are `eval_conversation_quality.attribute()["timelines"][handle]`
  dicts with keys `rowid, guid, from_me (bool), t (tz-aware datetime), text, atype`.

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_curator_population.py
"""Who the sleep-time curator reads (spec §3). Pure: no chat.db, no model."""
import datetime as dt
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import curator_population as cp  # noqa: E402

NOW = dt.datetime(2026, 9, 28, 12, 0, tzinfo=dt.timezone.utc)


def msgs(them, me, days_ago=1):
    t = NOW - dt.timedelta(days=days_ago)
    return ([{"from_me": False, "t": t} for _ in range(them)] +
            [{"from_me": True, "t": t} for _ in range(me)])


def test_threshold_boundaries():
    tl = {"+15550000001": msgs(10, 5), "+15550000002": msgs(9, 5),
          "+15550000003": msgs(10, 4)}
    assert cp.eligible_handles(tl, set(), NOW) == ["+15550000001"]


def test_window_edge_excludes_old_traffic():
    tl = {"+15550000004": msgs(10, 5, days_ago=31)}
    assert cp.eligible_handles(tl, set(), NOW) == []


def test_short_codes_never_eligible_and_email_is():
    tl = {"72975": msgs(50, 50), "friend@example.com": msgs(12, 6)}
    assert cp.eligible_handles(tl, set(), NOW) == ["friend@example.com"]
    assert cp.is_short_code("72975") and not cp.is_short_code("+15550000001")


def test_persona_contacts_are_skipped_by_the_wide_pass():
    tl = {"+15550000005": msgs(30, 30)}
    assert cp.eligible_handles(tl, {"+15550000005"}, NOW) == []


def test_suppressed_table_missing_means_empty(tmp_path):
    db = tmp_path / "m.db"
    sqlite3.connect(db).close()
    assert cp.load_suppressed(str(db)) == set()


def test_suppressed_handles_are_loaded(tmp_path):
    db = tmp_path / "m.db"
    c = sqlite3.connect(db)
    c.execute("CREATE TABLE contact_suppressions (contact TEXT, ts INT, reason TEXT, excerpt TEXT)")
    c.execute("INSERT INTO contact_suppressions VALUES ('+15550000006', 1, 'r', 'x')")
    c.commit(); c.close()
    assert cp.load_suppressed(str(db)) == {"+15550000006"}


def test_never_file_missing_empty_and_malformed_refuses(tmp_path):
    assert cp.load_never(str(tmp_path / "absent.json")) == set()
    good = tmp_path / "never.json"; good.write_text(json.dumps(["+15550000007"]))
    assert cp.load_never(str(good)) == {"+15550000007"}
    bad = tmp_path / "bad.json"; bad.write_text('{"not": "a list"}')
    try:
        cp.load_never(str(bad))
        assert False, "malformed never-file must refuse"
    except ValueError:
        pass


def test_exclusion_reason_order():
    assert cp.exclusion_reason("a", {"a"}, {"a"}) == "suppressed"
    assert cp.exclusion_reason("b", set(), {"b"}) == "never"
    assert cp.exclusion_reason("c", set(), set()) is None
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_curator_population.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'curator_population'`.

- [ ] **Step 3: Write the minimal implementation**

```python
# scripts/curator_population.py
"""Who the sleep-time curator reads each night (spec §3).

Pure functions over eval_conversation_quality.attribute() timelines, so the
selection is testable without chat.db or a model. The wide pass skips persona
contacts: the existing persona pass already curates them from memory.db.
"""
import datetime as dt
import json
import os
import re
import sqlite3

WINDOW_DAYS = 30
MIN_THEM = 10
MIN_ME = 5
NEVER_PATH = os.path.expanduser("~/.human/curator_never.json")


def is_short_code(handle):
    return bool(re.fullmatch(r"\d{3,6}", (handle or "").strip()))


def eligible_handles(timelines, persona_ids, now, window_days=WINDOW_DAYS,
                     min_them=MIN_THEM, min_me=MIN_ME):
    cutoff = now - dt.timedelta(days=window_days)
    out = []
    for handle, msgs in timelines.items():
        if not handle or is_short_code(handle) or handle in persona_ids:
            continue
        recent = [m for m in msgs if m["t"] >= cutoff]
        them = sum(1 for m in recent if not m["from_me"])
        me = sum(1 for m in recent if m["from_me"])
        if them >= min_them and me >= min_me:
            out.append(handle)
    return sorted(out)


def load_suppressed(mem_db_path):
    try:
        con = sqlite3.connect(f"file:{mem_db_path}?mode=ro", uri=True)
        rows = con.execute("SELECT contact FROM contact_suppressions").fetchall()
        con.close()
    except sqlite3.Error:
        return set()  # table not created yet = nobody has opted out
    return {r[0] for r in rows if r[0]}


def load_never(path=NEVER_PATH):
    if not os.path.exists(path):
        return set()
    data = json.load(open(path))
    if not isinstance(data, list) or not all(isinstance(x, str) for x in data):
        raise ValueError(f"{path}: expected a JSON list of handles")
    return set(data)


def exclusion_reason(handle, suppressed, never):
    if handle in suppressed:
        return "suppressed"
    if handle in never:
        return "never"
    return None
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_curator_population.py`
Expected: `8 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/curator_population.py tests/test_curator_population.py
git commit -m "feat(curator): nightly population — every 1:1 contact with real back-and-forth"
```

### Task 2: Evidence rows — chat.db turns with human-only citations

**Files:**
- Create: `scripts/curator_evidence.py`
- Test: `tests/test_curator_evidence.py`

**Interfaces:**
- Consumes: timeline dicts (Task 1) and `attribute()["labels"]`, a
  `{guid: "seth"|"huuman"|"ambiguous"}` map.
- Produces:
  - `chat_turn_rows(msgs, labels, n, cutoff) -> list[tuple[int, int, str, str]]`, oldest first, of `(rowid, t_ms, who, text)` with `who ∈ {"them","me","daemon"}`
  - `number_rows(rows) -> tuple[list[str], dict[int, tuple]]`, returning `(prompt lines, cite_map t-index → row)`
  - `parse_evidence(tokens) -> tuple[list[int], bool]`, returning `(t-indices, cited a daemon row)`

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_curator_evidence.py
"""Human-only evidence (spec §4): daemon/ambiguous sends are context, never citable."""
import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import curator_evidence as ce  # noqa: E402

T0 = dt.datetime(2026, 9, 27, 12, 0, tzinfo=dt.timezone.utc)


def m(rowid, guid, from_me, text, minutes=0):
    return {"rowid": rowid, "guid": guid, "from_me": from_me, "text": text,
            "t": T0 + dt.timedelta(minutes=minutes), "atype": 0}


def test_authorship_maps_to_who_and_ambiguous_is_daemon():
    msgs = [m(1, "a", False, "hi"), m(2, "b", True, "hey", 1), m(3, "c", True, "sup", 2),
            m(4, "d", True, "yo", 3)]
    labels = {"b": "seth", "c": "huuman", "d": "ambiguous"}
    rows = ce.chat_turn_rows(msgs, labels, 80, T0 - dt.timedelta(days=1))
    assert [r[2] for r in rows] == ["them", "me", "daemon", "daemon"]


def test_attachment_only_turn_is_a_placeholder_and_last_n_kept():
    msgs = [m(i, f"g{i}", False, "" if i == 5 else f"t{i}", i) for i in range(10)]
    rows = ce.chat_turn_rows(msgs, {}, 4, T0 - dt.timedelta(days=1))
    assert [r[0] for r in rows] == [6, 7, 8, 9]
    rows = ce.chat_turn_rows(msgs, {}, 80, T0 - dt.timedelta(days=1))
    assert rows[5][3] == "[attachment]"


def test_number_rows_daemon_rows_are_d_and_not_in_cite_map():
    rows = [(1, 0, "them", "hi"), (2, 0, "daemon", "hey"), (3, 0, "me", "ok")]
    lines, cite = ce.number_rows(rows)
    assert lines == ["[t0] them: hi", "[d0] me: hey", "[t1] me: ok"]
    assert set(cite) == {0, 1} and cite[1][0] == 3


def test_parse_evidence_flags_daemon_citations():
    assert ce.parse_evidence(["t0", "1", "[t2]"]) == ([0, 1, 2], False)
    assert ce.parse_evidence(["t0", "d1"]) == ([0], True)
    assert ce.parse_evidence([]) == ([], False)
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_curator_evidence.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'curator_evidence'`.

- [ ] **Step 3: Write the minimal implementation**

```python
# scripts/curator_evidence.py
"""Evidence rows for the sleep-time curator (spec §4-5).

Only the contact's messages and Seth's own sends are citable ([tN]). Daemon
sends and ambiguous ones are shown for context as [dN] and can never be
evidence: otherwise a detail the model confabulated in a reply becomes a
"memory" overnight and is repeated confidently (HaluMem, arXiv 2511.03506).
"""
import re


def chat_turn_rows(msgs, labels, n, cutoff):
    rows = []
    for msg in msgs:
        if msg["t"] < cutoff:
            continue
        if msg["from_me"]:
            who = "me" if labels.get(msg["guid"]) == "seth" else "daemon"
        else:
            who = "them"
        text = (msg.get("text") or "").strip().replace("\n", " ")[:300] or "[attachment]"
        rows.append((int(msg["rowid"]), int(msg["t"].timestamp() * 1000), who, text))
    return rows[-n:]


def number_rows(rows):
    lines, cite, t, d = [], {}, 0, 0
    for row in rows:
        _, _, who, text = row
        if who == "daemon":
            lines.append(f"[d{d}] me: {text}")
            d += 1
        else:
            lines.append(f"[t{t}] {who}: {text}")
            cite[t] = row
            t += 1
    return lines, cite


def parse_evidence(tokens):
    t_idx, daemon = [], False
    for tok in tokens or []:
        s = str(tok).strip().strip("[]").lower()
        if s.startswith("d"):
            daemon = True
            continue
        mt = re.fullmatch(r"t?(\d+)", s)
        if mt:
            t_idx.append(int(mt.group(1)))
    return t_idx, daemon
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_curator_evidence.py`
Expected: `4 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/curator_evidence.py tests/test_curator_evidence.py
git commit -m "feat(curator): chat.db evidence rows — only human messages are citable"
```

### Task 3: The "was it said?" name check and note validation

**Files:**
- Modify: `scripts/curator_evidence.py` (append)
- Test: `tests/test_curator_evidence.py` (append)

**Interfaces:**
- Consumes: `cite_map` and `parse_evidence` (Task 2); a note dict with keys
  `note, kind, confidence, evidence_tokens (list[str]), names (list[{"name","type"}])` (Task 4).
- Produces:
  - `name_said(name: str, texts: list[str]) -> bool`
  - `validate_note(note, cite_map) -> tuple[dict | None, str]`, returning
    `(note with "evidence_rows" set | None, reason)`, where reason is one of
    `ok`, `no_evidence`, `daemon_evidence`, `name_not_said`

- [ ] **Step 1: Write the failing tests (append)**

```python
def test_name_said_word_bounded_and_possessive():
    assert ce.name_said("Priya", ["priya's surgery is tuesday"])
    assert not ce.name_said("Al", ["Also, see you then"])
    assert ce.name_said("St Petersburg", ["moving to st petersburg soon"])
    assert not ce.name_said("Priya", ["[attachment]"])


def _cite():
    return {0: (11, 1, "them", "priya's surgery is tuesday"), 1: (12, 2, "me", "hope it goes well")}


def test_validate_note_accepts_supported_named_note():
    note = {"note": "Priya's surgery tuesday", "kind": "plan", "confidence": 0.9,
            "evidence_tokens": ["t0"], "names": [{"name": "Priya", "type": "person"}]}
    out, why = ce.validate_note(note, _cite())
    assert why == "ok" and out["evidence_rows"] == [_cite()[0]]


def test_validate_note_rejects_daemon_empty_and_invented_names():
    base = {"note": "x", "kind": "fact", "confidence": 0.9, "names": []}
    assert ce.validate_note({**base, "evidence_tokens": ["d0"]}, _cite())[1] == "daemon_evidence"
    assert ce.validate_note({**base, "evidence_tokens": ["t9"]}, _cite())[1] == "no_evidence"
    inv = {**base, "evidence_tokens": ["t0"], "names": [{"name": "Marcus", "type": "person"}]}
    assert ce.validate_note(inv, _cite()) == (None, "name_not_said")
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_curator_evidence.py`
Expected: FAIL with `AttributeError: module 'curator_evidence' has no attribute 'name_said'`.

- [ ] **Step 3: Write the implementation (append to `scripts/curator_evidence.py`)**

```python
def name_said(name, texts):
    """Verbatim, case-insensitive, word-bounded: 'Al' is not in 'Also',
    'Priya' is in "priya's"."""
    name = (name or "").strip()
    if not name:
        return False
    pat = re.compile(r"(?<![A-Za-z0-9])" + re.escape(name) + r"(?![A-Za-z0-9])", re.I)
    return any(pat.search(t or "") for t in texts)


def validate_note(note, cite_map):
    t_idx, daemon = parse_evidence(note.get("evidence_tokens"))
    if daemon:
        return None, "daemon_evidence"
    rows = [cite_map[i] for i in t_idx if i in cite_map]
    if not rows:
        return None, "no_evidence"
    texts = [r[3] for r in rows]
    for n in note.get("names") or []:
        if not name_said(n.get("name"), texts):
            return None, "name_not_said"
    return {**note, "evidence_rows": rows}, "ok"
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_curator_evidence.py`
Expected: `7 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/curator_evidence.py tests/test_curator_evidence.py
git commit -m "feat(curator): deterministic was-it-said check before model verification"
```

### Task 4: Prompt keeps names capitalized; `parse_notes` returns `names` and raw evidence tokens

**Files:**
- Modify: `scripts/insight_stream.py`, `build_prompt` (≈ lines 143–159) and `parse_notes` (≈ lines 177–210)
- Test: `tests/test_insight_stream_names.py`

**Interfaces:**
- Produces: `parse_notes(text, max_notes)` notes gain `"names": [{"name": str, "type": str}]`
  (types ⊂ {person, place, org, event}) and `"evidence_tokens": [str]`. The existing
  `"evidence": [int]` is kept, so the persona pass and the 17 existing tests are unchanged.

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_insight_stream_names.py
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
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_insight_stream_names.py`
Expected: FAIL on `"Lowercase, like a note to yourself" not in system` and on `KeyError: 'evidence_tokens'`.

- [ ] **Step 3: Implement**

In `build_prompt`, replace:

```python
        "anything not in the texts. Lowercase, like a note to yourself, present tense, each under "
        "110 characters.\n\n"
```

with:

```python
        "anything not in the texts. Casual lowercase like a note to yourself, but keep names of "
        "people, places and organizations capitalized as written; present tense, each under "
        "110 characters.\n\n"
```

and replace the output-spec line:

```python
        "\"confidence\": number 0-1, \"evidence\": [the [tN] numbers of the texts the note "
        "comes from]}. No prose before or after."
```

with:

```python
        "\"confidence\": number 0-1, \"evidence\": [the [tN] labels of the texts the note "
        "comes from; never a [dN]], \"names\": [{\"name\": str, \"type\": \"person\"|\"place\"|"
        "\"org\"|\"event\"} for every specific name in the note]}. No prose before or after."
```

In `parse_notes`, just before `seen.add(note.lower())`, add:

```python
        raw_ev = o.get("evidence") if isinstance(o.get("evidence"), list) else []
        names = []
        for nm in (o.get("names") if isinstance(o.get("names"), list) else []):
            if not isinstance(nm, dict):
                continue
            nname = str(nm.get("name") or "").strip()
            ntype = str(nm.get("type") or "").strip().lower()
            if nname and len(nname) <= 60 and ntype in {"person", "place", "org", "event"}:
                names.append({"name": nname, "type": ntype})
```

and change the `notes.append(...)` line to:

```python
        notes.append({"note": note, "kind": kind, "confidence": conf, "evidence": ev,
                      "evidence_tokens": [str(e) for e in raw_ev], "names": names})
```

- [ ] **Step 4: Run the tests; the new and existing tests should pass**

Run: `python3 -m pytest -q tests/test_insight_stream_names.py tests/test_insight_stream_admission.py tests/test_insight_stream_keywords.py`
Expected: `19 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/insight_stream.py tests/test_insight_stream_names.py
git commit -m "feat(insight-stream): keep proper nouns capitalized; structured names per note"
```

### Task 5: `--population wide`, the extraction pass

**Files:**
- Modify: `scripts/insight_stream.py` (new `wide_pass`, argparse flags, dispatch in `main`)
- Test: `tests/test_insight_stream_wide.py`

**Interfaces:**
- Consumes: Tasks 1–4; the existing `call_model`, `verify_claims`, `admit`,
  `source_tag`, `build_prompt`, `VERIFY_SYSTEM`, `SCHEMA` and `migrate`;
  `eval_conversation_quality.attribute`.
- Produces:
  - `wide_pass(db, a, identity, persona_ids, att, now, write) -> dict`, the counts
    manifest with keys `eligible, excluded_suppressed, excluded_never, curated,
    notes_written, rejected_no_evidence, rejected_daemon_evidence,
    rejected_name_not_said, names_total, notes_named`
  - Written row: `source = "curator_wide:" + source_tag(k, agree)`,
    `evidence_ids = JSON ["chat:<rowid>", …]`, and `as_of_ms` = newest evidence
    time.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_insight_stream_wide.py
"""--population wide: human-only evidence, curator_wide source, counts-only manifest."""
import datetime as dt
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import insight_stream as ins  # noqa: E402

NOW = dt.datetime(2026, 9, 28, 12, 0, tzinfo=dt.timezone.utc)
H = "+15550000042"


def timeline():
    out = []
    for i in range(12):
        out.append({"rowid": 100 + i, "guid": f"them{i}", "from_me": False,
                    "text": "priya's surgery is tuesday" if i == 0 else f"them {i}",
                    "t": NOW - dt.timedelta(hours=40 - i), "atype": 0})
    for i in range(6):
        out.append({"rowid": 200 + i, "guid": f"me{i}", "from_me": True, "text": f"me {i}",
                    "t": NOW - dt.timedelta(hours=20 - i), "atype": 0})
    out.append({"rowid": 300, "guid": "bot", "from_me": True, "text": "Marcus says hi",
                "t": NOW - dt.timedelta(hours=1), "atype": 0})
    return out


class A:
    consistency_k = 1; turns = 80; min_turns = 10; max_notes = 8
    url = "http://127.0.0.1:1/x"; model = "m"
    never_path = "/nonexistent/curator_never.json"  # hermetic: never read the real file


import pytest  # noqa: E402


@pytest.fixture(autouse=True)
def _hermetic_memory_db(tmp_path, monkeypatch):
    """wide_pass reads suppressions from MEMORY_DB; never touch the real ~/.human."""
    monkeypatch.setattr(ins, "MEMORY_DB", str(tmp_path / "suppressions.db"))


def test_wide_pass_writes_only_supported_named_notes(tmp_path, monkeypatch):
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    att = {"timelines": {H: timeline()},
           "labels": {**{f"me{i}": "seth" for i in range(6)}, "bot": "huuman"}}
    # the model proposes: one good note, one citing the daemon row, one with an invented name
    raw = json.dumps([
        {"note": "Priya surgery tuesday", "kind": "plan", "confidence": 0.9,
         "evidence": ["t0"], "names": [{"name": "Priya", "type": "person"}]},
        {"note": "Marcus says hi", "kind": "fact", "confidence": 0.9,
         "evidence": ["d0"], "names": [{"name": "Marcus", "type": "person"}]},
        {"note": "Dana visiting", "kind": "fact", "confidence": 0.9,
         "evidence": ["t1"], "names": [{"name": "Dana", "type": "person"}]}])
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: raw)
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [1] * len(claims))
    man = ins.wide_pass(db, A(), "id", set(), att, NOW, write=True)
    rows = db.execute("SELECT insight, source, evidence_ids FROM contact_insights").fetchall()
    assert rows == [("Priya surgery tuesday", "curator_wide:" + ins.source_tag(1, 1),
                     json.dumps(["chat:100"]))]
    assert man["eligible"] == 1 and man["notes_written"] == 1
    assert man["rejected_daemon_evidence"] == 1 and man["rejected_name_not_said"] == 1
    assert "Priya" not in json.dumps(man)  # counts only, never text


def test_wide_pass_dry_run_writes_nothing(tmp_path, monkeypatch):
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    att = {"timelines": {H: timeline()}, "labels": {f"me{i}": "seth" for i in range(6)}}
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: "[]")
    monkeypatch.setattr(ins, "verify_claims", lambda a, s, u, claims, k: [])
    ins.wide_pass(db, A(), "id", set(), att, NOW, write=False)
    assert db.execute("SELECT COUNT(*) FROM contact_insights").fetchone()[0] == 0
```

- [ ] **Step 2: Run the test; it should fail**

Run: `python3 -m pytest -q tests/test_insight_stream_wide.py`
Expected: FAIL with `AttributeError: module 'insight_stream' has no attribute 'wide_pass'`.

- [ ] **Step 3: Implement `wide_pass` (add above `def main():`)**

```python
import curator_evidence as ce  # noqa: E402  (scripts/ is on sys.path when run as a script)
import curator_population as cp  # noqa: E402

WIDE_SOURCE_PREFIX = "curator_wide:"


def wide_pass(db, a, identity, persona_ids, att, now, write):
    """Curate every eligible non-persona 1:1 contact from chat.db (spec §3-5).
    Returns a counts-only manifest; never text."""
    man = {k: 0 for k in ("eligible", "excluded_suppressed", "excluded_never", "curated",
                          "notes_written", "rejected_no_evidence", "rejected_daemon_evidence",
                          "rejected_name_not_said", "names_total", "notes_named")}
    suppressed = cp.load_suppressed(MEMORY_DB)  # module global so tests can redirect it
    never = cp.load_never(getattr(a, "never_path", cp.NEVER_PATH))
    cutoff = now - dt.timedelta(days=getattr(a, "window_days", cp.WINDOW_DAYS))
    handles = cp.eligible_handles(att["timelines"], persona_ids, now)
    man["eligible"] = len(handles)
    now_ms = int(now.timestamp() * 1000)
    for h in handles:
        why = cp.exclusion_reason(h, suppressed, never)
        if why:
            man[f"excluded_{why}"] += 1
            continue
        rows = ce.chat_turn_rows(att["timelines"][h], att["labels"], a.turns, cutoff)
        if len(rows) < a.min_turns:
            continue
        lines, cite = ce.number_rows(rows)
        system, user = build_prompt(identity, h, "", lines, a.max_notes)
        notes = parse_notes(call_model(a.url, a.model, system, user), a.max_notes)
        agree = verify_claims(a, VERIFY_SYSTEM.format(
            identity=identity, name=h, rel="",
            question="A note is supported only if a specific text states it."),
            user, [n["note"] for n in notes], a.consistency_k)
        notes = admit(notes, agree, a.consistency_k, label=h)
        kept = []
        for n in notes:
            v, reason = ce.validate_note(n, cite)
            if v is None:
                man[f"rejected_{reason}"] += 1
                continue
            man["names_total"] += len(v["names"])
            man["notes_named"] += 1 if v["names"] else 0
            kept.append(v)
        man["curated"] += 1
        if write and kept:
            before = db.total_changes
            db.executemany(
                "INSERT OR IGNORE INTO contact_insights (contact_id, kind, insight, confidence,"
                " as_of_ms, source, created_at_ms, evidence_ids) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                [(h, v["kind"], v["note"], v["confidence"],
                  max(r[1] for r in v["evidence_rows"]),
                  WIDE_SOURCE_PREFIX + source_tag(a.consistency_k, v["agree"]), now_ms,
                  json.dumps([f"chat:{r[0]}" for r in v["evidence_rows"]])) for v in kept])
            db.commit()
            man["notes_written"] += db.total_changes - before
    return man
```

Also add `import datetime as dt` to the imports at the top of `insight_stream.py` if
it isn't already there.

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_insight_stream_wide.py tests/test_insight_stream_*.py`
Expected: all pass (`21 passed`).

- [ ] **Step 5: Commit**

```bash
git add scripts/insight_stream.py tests/test_insight_stream_wide.py
git commit -m "feat(insight-stream): --population wide pass over chat.db (curator_wide source)"
```

### Task 6: CLI wiring, opt-out retirement, deadline and resume, refusals, manifest

**Files:**
- Modify: `scripts/insight_stream.py`: `main()` argparse and dispatch, plus helpers
  `retire_suppressed`, `order_by_last_run` and `write_manifest`
- Test: `tests/test_insight_stream_wide.py` (append)

**Interfaces:**
- Consumes: `wide_pass` (Task 5), `cp.load_suppressed`.
- Produces: flags `--population {persona,wide}` (default `persona`), `--chat-db`,
  `--window-days`, `--deadline HH:MM`, `--manifest-dir`, `--never-path`; state file
  `~/.human/curator_state.json` (`{handle: last_run_ms}`); manifest
  `<manifest-dir>/curator-manifest-YYYYMMDD.json`. `wide_pass` takes an optional
  `deadline` (datetime or None) and `state` (dict).

- [ ] **Step 1: Write the failing tests (append)**

```python
def test_suppressed_contact_rows_are_retired_whatever_the_source(tmp_path, monkeypatch):
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    db.execute("INSERT INTO contact_insights (contact_id, kind, insight, confidence, as_of_ms,"
               " source, created_at_ms) VALUES (?, 'fact', 'old note', 0.9, 1, 'extractor:v2', 1)",
               (H,))
    db.commit()
    n = ins.retire_suppressed(db, {H}, now_ms=5, write=True)
    assert n == 1
    assert db.execute("SELECT retired_at_ms FROM contact_insights").fetchone()[0] == 5


def test_deadline_stops_before_the_next_contact_and_order_prefers_stale(tmp_path, monkeypatch):
    state = {"+1a": 300, "+1b": 100, "+1c": 200}
    assert ins.order_by_last_run(["+1a", "+1b", "+1c", "+1d"], state) == ["+1d", "+1b", "+1c", "+1a"]
    db = sqlite3.connect(tmp_path / "m.db"); db.executescript(ins.SCHEMA); ins.migrate(db)
    att = {"timelines": {H: timeline()}, "labels": {f"me{i}": "seth" for i in range(6)}}
    monkeypatch.setattr(ins, "call_model", lambda *a, **k: (_ for _ in ()).throw(AssertionError))
    man = ins.wide_pass(db, A(), "id", set(), att, NOW, write=True,
                        deadline=NOW - dt.timedelta(minutes=1), state={})
    assert man["curated"] == 0 and man["stopped_at_deadline"] == 1


def test_manifest_refuses_when_nothing_is_eligible(tmp_path):
    assert ins.write_manifest(str(tmp_path), NOW, {"eligible": 0}) == 2
    assert list(tmp_path.iterdir()) == []
    assert ins.write_manifest(str(tmp_path), NOW, {"eligible": 3, "notes_written": 1}) == 0
    assert json.loads((tmp_path / "curator-manifest-20260928.json").read_text())["eligible"] == 3
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_insight_stream_wide.py`
Expected: FAIL with `AttributeError: … has no attribute 'retire_suppressed'`.

- [ ] **Step 3: Implement**

Add the helpers above `wide_pass`:

```python
CURATOR_STATE = os.path.join(HOME, ".human/curator_state.json")


def retire_suppressed(db, suppressed, now_ms, write):
    """Opt-out takes effect on what is already known, not only on what is
    learned next: every live row for a suppressed contact is retired."""
    if not suppressed:
        return 0
    qs = ",".join("?" * len(suppressed))
    n = db.execute(f"SELECT COUNT(*) FROM contact_insights WHERE retired_at_ms=0 AND"
                   f" contact_id IN ({qs})", tuple(suppressed)).fetchone()[0]
    if write and n:
        db.execute(f"UPDATE contact_insights SET retired_at_ms=? WHERE retired_at_ms=0 AND"
                   f" contact_id IN ({qs})", (now_ms, *suppressed))
        db.commit()
    return n


def order_by_last_run(handles, state):
    return sorted(handles, key=lambda h: (state.get(h, 0), h))


def write_manifest(manifest_dir, now, man):
    if not man.get("eligible"):
        print("refusing: 0 eligible contacts (no manifest written)", file=sys.stderr)
        return 2
    os.makedirs(manifest_dir, exist_ok=True)
    path = os.path.join(manifest_dir, f"curator-manifest-{now.strftime('%Y%m%d')}.json")
    json.dump(man, open(path, "w"), indent=1, sort_keys=True)
    return 0
```

Change the `wide_pass` signature to
`def wide_pass(db, a, identity, persona_ids, att, now, write, deadline=None, state=None):`,
add `"stopped_at_deadline"` to the manifest keys, replace
`for h in handles:` with
`for h in order_by_last_run(handles, state if state is not None else {}):`, and add
as the first statement of that loop body:

```python
        if deadline is not None and dt.datetime.now(dt.timezone.utc) >= deadline:
            man["stopped_at_deadline"] = 1
            break
```

After the write for a contact (inside the loop), add
`if state is not None: state[h] = now_ms`.

In `main()`, add the arguments:

```python
    ap.add_argument("--population", choices=["persona", "wide"], default="persona")
    ap.add_argument("--chat-db", default=os.path.join(HOME, "Library/Messages/chat.db"))
    ap.add_argument("--window-days", type=int, default=30)
    ap.add_argument("--deadline", help="HH:MM local; stop before the next contact after this")
    ap.add_argument("--manifest-dir", default=os.path.join(HOME, ".human/logs"))
    ap.add_argument("--never-path", default=os.path.join(HOME, ".human/curator_never.json"))
```

and, after `now_ms = …`, dispatch:

```python
    if a.population == "wide":
        import eval_conversation_quality as cq
        try:
            con = sqlite3.connect(f"file:{a.chat_db}?mode=ro", uri=True)
            con.execute("SELECT 1 FROM message LIMIT 1").fetchall(); con.close()
        except sqlite3.Error as e:
            print(f"refusing: chat.db unreadable ({e}); grant Full Disk Access to this "
                  "python for the launchd job", file=sys.stderr)
            return 2
        try:
            urllib.request.urlopen(a.url.rsplit("/v1/", 1)[0] + "/health", timeout=5)
        except Exception as e:
            print(f"refusing: model server down ({e})", file=sys.stderr)
            return 2
        try:
            cp.load_never(a.never_path)
        except (ValueError, json.JSONDecodeError) as e:
            print(f"refusing: {e}", file=sys.stderr)
            return 2
        now = dt.datetime.now(dt.timezone.utc)
        att = cq.attribute(a.chat_db, MEMORY_DB, now - dt.timedelta(days=a.window_days))
        retired = retire_suppressed(db, cp.load_suppressed(MEMORY_DB), now_ms, a.write)
        deadline = None
        if a.deadline:
            hh, mm = map(int, a.deadline.split(":"))
            local = dt.datetime.now().astimezone().replace(hour=hh, minute=mm, second=0,
                                                           microsecond=0)
            deadline = local.astimezone(dt.timezone.utc)
        state = json.load(open(CURATOR_STATE)) if os.path.exists(CURATOR_STATE) else {}
        man = wide_pass(db, a, identity, set(contacts), att, now, a.write, deadline, state)
        man["retired_suppressed"] = retired
        if a.write:
            json.dump(state, open(CURATOR_STATE, "w"))
        return write_manifest(a.manifest_dir, now, man)
```

(`urllib.request`, `json`, `os`, `sys` and `sqlite3` are already imported in
`insight_stream.py`; add `import urllib.request` if not.)

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_insight_stream_*.py tests/test_curator_*.py`
Expected: all pass.

- [ ] **Step 5: Dry run against real data (read-only, no writes)**

Run: `python3 scripts/insight_stream.py --population wide --consistency-k 3 --manifest-dir /tmp/curator-dry`
Expected: exit 0, and `/tmp/curator-dry/curator-manifest-*.json` with `eligible ≥ 1` and
`notes_written == 0` (no `--write`). Paste the manifest into the PR, since it contains
counts only.

- [ ] **Step 6: Commit**

```bash
git add scripts/insight_stream.py tests/test_insight_stream_wide.py
git commit -m "feat(insight-stream): wide-pass CLI — opt-out retirement, deadline/resume, refusals, manifest"
```

### Task 7: Render gate `HU_INSIGHT_WIDE` in C

**Files:**
- Modify: `src/memory/repos/contact_insights_repo_sqlite.c`, `hu_contact_insights_render` (≈ lines 101–160)
- Test: `tests/test_contact_insights_repo.c`

**Interfaces:**
- Consumes: `hu_gate_mode_from_env` (`include/human/core/gate_mode.h`),
  `hu_contact_insights_add` (which stores `source`).
- Produces: under `off`/`shadow`, rows whose `source` starts with `curator_wide` are
  not rendered; `shadow` logs `insight_wide shadow: N rows (not rendered)`; `live`
  renders them. Rows from other sources render exactly as today.

- [ ] **Step 1: Write the failing test** (add to `tests/test_contact_insights_repo.c`,
  register it next to `render_orders_newest_first_with_month_and_caps`)

```c
/* HU_INSIGHT_WIDE gates ONLY curator_wide rows; persona rows are unaffected. */
static void curator_wide_rows_follow_the_insight_wide_gate(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "fact",
                                         "persona note Initech", 0.9, 1767225600000LL,
                                         "extractor:v2:k3:a3", NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_contact_insights_add(&mem, k_contact, strlen(k_contact), "plan",
                                         "wide note Priya surgery", 0.9, 1767225600001LL,
                                         "curator_wide:extractor:v2:k3:a3", NULL),
                 HU_OK);
    const char *modes[] = {NULL, "shadow", "garbage", "live"};
    const bool wide_seen[] = {false, false, false, true};
    for (size_t i = 0; i < 4; i++) {
        if (modes[i])
            setenv("HU_INSIGHT_WIDE", modes[i], 1);
        else
            unsetenv("HU_INSIGHT_WIDE");
        char *out = NULL;
        size_t len = 0;
        HU_ASSERT_EQ(hu_contact_insights_render(&mem, &a, k_contact, strlen(k_contact), 8, 900,
                                                0.5, &out, &len),
                     HU_OK);
        HU_ASSERT_NOT_NULL(out);
        HU_ASSERT_NOT_NULL(strstr(out, "persona note Initech"));
        HU_ASSERT_EQ(strstr(out, "Priya") != NULL, wide_seen[i]);
        a.free(a.ctx, out, len + 1);
    }
    unsetenv("HU_INSIGHT_WIDE");
    mem.vtable->deinit(mem.ctx);
}
```


- [ ] **Step 2: Build and run; it should fail**

Run: `cmake --build build --target human_tests -j12 && ./build/human_tests --filter=curator_wide_rows`
Expected: FAIL on the `strstr(out, "Priya") != NULL` assertion for the unset/shadow cases.

- [ ] **Step 3: Implement.** In `hu_contact_insights_render`, change the SQL and
  binding, and add the shadow log:

```c
    /* Curator-wide rows (source 'curator_wide:…') render only when
     * HU_INSIGHT_WIDE=live. Activation gated on a blind A/B (specificity,
     * detection non-inferior); default OFF. */
    hu_gate_mode_t wide = hu_gate_mode_from_env("HU_INSIGHT_WIDE", HU_GATE_OFF);
    const char *sql = "SELECT insight, as_of_ms FROM contact_insights"
                      " WHERE contact_id = ?1 AND retired_at_ms = 0 AND confidence >= ?2"
                      " AND (?4 OR source IS NULL OR source NOT LIKE 'curator_wide%')"
                      " ORDER BY as_of_ms DESC, id DESC LIMIT ?3";
```

and after binding `?3`: `sqlite3_bind_int(st, 4, wide == HU_GATE_LIVE ? 1 : 0);`.
Before `return` at the end of the function, when `wide == HU_GATE_SHADOW`, count the
excluded rows and log:

```c
    if (wide == HU_GATE_SHADOW) {
        sqlite3_stmt *c = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT COUNT(*) FROM contact_insights WHERE contact_id = ?1 AND"
                " retired_at_ms = 0 AND confidence >= ?2 AND source LIKE 'curator_wide%'",
                -1, &c, NULL) == SQLITE_OK) {
            sqlite3_bind_text(c, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
            sqlite3_bind_double(c, 2, min_confidence);
            if (sqlite3_step(c) == SQLITE_ROW && sqlite3_column_int(c, 0) > 0)
                hu_log_info("insight_wide", NULL, "shadow: %d rows (not rendered)",
                            sqlite3_column_int(c, 0));
            sqlite3_finalize(c);
        }
    }
```

Add `#include "human/core/gate_mode.h"` and `#include "human/core/log.h"` at the top
if missing.

- [ ] **Step 4: Build and run; it should pass, then run the full suite**

Run: `cmake --build build --target human_tests -j12 && ./build/human_tests --filter=contact_insights && ./build/human_tests 2>&1 | tail -3`
Expected: the new test passes, the existing 4 render tests pass, and the full suite shows 0 failures.

- [ ] **Step 5: Commit**

```bash
git add src/memory/repos/contact_insights_repo_sqlite.c tests/test_contact_insights_repo.c
git commit -m "feat(insights): HU_INSIGHT_WIDE render gate for curator_wide rows (default off)"
```

### Task 8: CI coverage for the Python curator tests, and the nightly command

**Files:**
- Modify: `.github/workflows/ci.yml` (next to the persona-evolution pytest step, ≈ line 1200)

- [ ] **Step 1: Add the CI step** (after `Install pytest for the persona-evolution suite`):

```yaml
      - name: Insight-stream + curator pins (hermetic, no chat.db, no model)
        run: python3 -m pytest -q tests/test_insight_stream_*.py tests/test_curator_*.py
```

- [ ] **Step 2: Verify locally**

Run: `python3 -m pytest -q tests/test_insight_stream_*.py tests/test_curator_*.py`
Expected: all pass.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: run the insight-stream and curator tests (17 existing tests had no CI step)"
```

- [ ] **Step 4: Operator step, after merge and deploy (not in the repo).** The plist
  `~/Library/LaunchAgents/ai.human.insight-nightly.plist` runs from the shared main
  checkout. Append `--population wide` as a separate command, **with `--write`** so rows
  land but stay unrendered while `HU_INSIGHT_WIDE` is unset:

```
&& /opt/homebrew/bin/python3 scripts/insight_stream.py --population wide --consistency-k 3 --deadline 07:30 --write
```

Back up the plist first (`cp …plist …plist.bak-curator-wide-$(date +%Y%m%d-%H%M%S)`),
then `launchctl bootout gui/$UID <plist> && launchctl bootstrap gui/$UID <plist>`, and
confirm with `launchctl print gui/$UID/ai.human.insight-nightly | grep -c population`.

---

## After this plan

- Run nightly with `HU_INSIGHT_WIDE` unset, read the manifests, then set it to
  `shadow`, and promote only per spec §6 targets plus a blind A/B.
- Step C (mirroring names into graph.db) gets its own plan once A has shadow data.
- Spec §6's "dated `plan` notes → prospective memories" target needs the existing
  `--prospective` pass to cover wide contacts too. That's a small follow-up, deferred so
  this plan stays one concern; the manifest target is measured once it lands.
- Noticed while planning, **out of scope**: the existing persona pass numbers
  memory.db `assistant` rows as citable `[tN]`, so daemon text can be cited as
  evidence there too. Candidate follow-up: apply Task 2's citation rule to the
  persona pass.
