---
title: Second-opinion lane — implementation plan
date: 2026-09-29
status: draft (awaiting review)
---

# Second-opinion lane — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task by task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A local, overnight second opinion from a different model family (Gemma 4 31B on `:8743`). It audits the curator's kept notes, calibrates the blind-A/B judge, and critiques weak replies (with reference replies where Seth didn't reply). Every result is stored with provenance, and nothing reaches live replies.

**Architecture:** A new Python package, `scripts/second_opinion/`. It has pure, testable units:
- `store` and `stats`: the lane's SQLite store and measurement math;
- `backend`: Gemma local by default, with Vertex opt-in;
- one module per job: `audit`, `judge`, `gold`;
- small calibration and rating CLIs;
- `run_nightly`, which owns the lock, the deadline, refusals, serving and the manifest.

Tests are hermetic: a fake backend, temporary SQLite files, no network and no chat.db.

**Tech stack:** Python 3 stdlib plus pytest; SQLite; `mlx_lm.server` from `~/Documents/gemma-realtime-1/.venv312` (mlx_lm 0.31.3, which has the gemma4 architecture); Vertex AI REST with ADC (opt-in).

**Spec:** `docs/superpowers/specs/2026-09-29-second-opinion-lane-design.md`

## Global constraints

- **Model:** `mlx-community/gemma-4-31b-it-4bit`, served by `mlx_lm.server` on `127.0.0.1:8743`. The runner starts it and always stops it; it is never always-on.
- **Default backend is local.** `VertexBackend` (`gemini-3.8-flash`, project `johnb-2025`, `locations/global`) only runs when constructed with `allow=True`, which happens only via `--backend vertex`. It prints a one-line stderr notice that message excerpts leave the Mac. `generationConfig.thinkingConfig.thinkingBudget` is always set explicitly.
- `GemmaBackend` refuses any non-loopback URL.
- **Reads:** memory.db and chat.db only (`mode=ro`).
- **Writes:** only `~/.human/second_opinion.db` (mode 0600), manifests, reports and sheets.
- The manifest and reports hold **counts only**: no message text, note text or handles.
- **Refusals** (exit 2, write nothing):
  - Gemma not healthy within 600 s;
  - `:8743` already serving;
  - chat.db or memory.db unreadable;
  - `VertexBackend` without `allow`.
- **Errors and stops:**
  - One item failing is counted and skipped.
  - If every attempted item fails, exit 3, and the manifest is still written.
  - A second concurrent run exits 0 and does nothing.
  - The deadline follows `insight_stream.resolve_deadline` (window closed → exit 0, write nothing).
- **Never a number without a measurement:** `n = 0` gives `"not measured"`, and κ needs n ≥ 20.
- **Provenance:** every stored row records `backend`, `prompt_version` and `created_at_ms`. Judge results go through `score.py --rater synthetic`, never `human`.
- Reference replies are stored with `rated = NULL`, and export includes only `rated = 1`.
- **Out of scope:** training, the live reply path, K3, and changing `synthetic_judge.py`'s prompt.
- **Imports:** tests put `scripts/` on `sys.path` and `import second_opinion.<module>`. The runner is invoked as `cd scripts && python3 -m second_opinion.run_nightly`.

## Review focus

1. **Another session's server already on `:8743`.** The runner must refuse, not share it or kill it. (Test in Task 2.)
2. **A curator note whose evidence was deleted or is malformed** (`evidence_ids` null, bad JSON, a dangling rowid). It must be skipped and counted, never judged without evidence. (Test in Task 3.)
3. **Gemma returns chatter instead of a verdict** ("Sure! Here's my analysis…"). It must be stored as `unclear` and counted, never guessed. (Tests in Tasks 3 and 6.)
4. **A rating sheet that no human has filled in yet** (empty `choice` column). It must contribute no weak moments and no calibration pairs. (Tests in Tasks 5 and 6.)
5. **The same item seen on consecutive nights.** Audits, critiques and reference replies are deduplicated per backend, so a second night never re-spends model time on them. (Tests in Tasks 3 and 6.)

---

### Task 1: Foundation — the store and the measurement math

**Files:**
- Create: `scripts/second_opinion/__init__.py`, `scripts/second_opinion/store.py`, `scripts/second_opinion/stats.py`
- Test: `tests/test_second_opinion_store.py`, `tests/test_second_opinion_stats.py`

**Interfaces:**
- Produces:
  - `store.open_store(path) -> sqlite3.Connection`: creates the schema; the file is chmod 0600; `":memory:"` is allowed.
  - `store.now_ms() -> int`
  - `store.add_audit(con, insight_id, source, verdict, reason, unparseable, backend, prompt_version, at_ms) -> int` (rows inserted, 0 or 1)
  - `store.audited_ids(con, backend) -> set[int]`
  - `store.add_critique(con, item_id, weak_source, gaps, missing, severity, unparseable, backend, prompt_version, at_ms) -> int`, where `gaps` is a list and is stored as JSON
  - `store.critiqued_items(con, backend) -> set[str]`
  - `store.add_reference(con, contact_id, chat_rowid, context, reply, backend, prompt_version, at_ms) -> int`
  - `store.referenced_rowids(con, backend) -> set[int]`
  - `store.start_run(con, backend, jobs, at_ms) -> int`; `store.finish_run(con, run_id, exit_code, manifest_json, at_ms) -> None`
  - `stats.NOT_MEASURED = "not measured"`
  - `stats.wilson(k, n) -> tuple[float, float] | None`
  - `stats.rate(k, n, min_n=1) -> dict | str`, returning `{"k","n","rate","ci95":[lo,hi]}` or `NOT_MEASURED`
  - `stats.cohen_kappa(pairs) -> float | None`

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_second_opinion_stats.py
"""Measurement math for the second-opinion lane (spec §4, §5)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import stats  # noqa: E402


def test_wilson_matches_known_values():
    lo, hi = stats.wilson(5, 10)
    assert abs(lo - 0.2366) < 1e-3 and abs(hi - 0.7634) < 1e-3
    lo, hi = stats.wilson(0, 10)
    assert lo < 1e-3 and abs(hi - 0.2775) < 1e-3
    assert stats.wilson(0, 0) is None


def test_rate_never_reports_a_number_without_a_denominator():
    assert stats.rate(0, 0) == stats.NOT_MEASURED
    assert stats.rate(3, 10, min_n=20) == stats.NOT_MEASURED
    r = stats.rate(3, 10)
    assert r["k"] == 3 and r["n"] == 10 and r["rate"] == 0.3 and len(r["ci95"]) == 2


def test_cohen_kappa_chance_and_perfect():
    assert stats.cohen_kappa([]) is None
    assert stats.cohen_kappa([("A", "A"), ("B", "B")]) == 1.0
    assert abs(stats.cohen_kappa([("A", "A"), ("B", "B"), ("A", "B"), ("B", "A")])) < 1e-9
```

```python
# tests/test_second_opinion_store.py
"""second_opinion.db: provenance on every row, dedupe per backend, private file."""
import json
import os
import stat
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import store  # noqa: E402


def test_store_file_is_private_and_schema_exists(tmp_path):
    p = tmp_path / "so.db"
    con = store.open_store(str(p))
    assert stat.S_IMODE(os.stat(p).st_mode) == 0o600
    tables = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"audits", "critiques", "reference_replies", "runs"} <= tables


def test_audits_dedupe_per_backend_and_carry_provenance():
    con = store.open_store(":memory:")
    assert store.add_audit(con, 7, "wide", "supported", "ok", 0, "g@local", "audit-v1", 1) == 1
    assert store.add_audit(con, 7, "wide", "unsupported", "x", 0, "g@local", "audit-v1", 2) == 0
    assert store.add_audit(con, 7, "wide", "unsupported", "x", 0, "v@vertex", "audit-v1", 3) == 1
    assert store.audited_ids(con, "g@local") == {7}
    row = con.execute("SELECT backend, prompt_version, created_at_ms FROM audits "
                      "WHERE backend='g@local'").fetchone()
    assert row == ("g@local", "audit-v1", 1)


def test_critiques_and_references_dedupe():
    con = store.open_store(":memory:")
    assert store.add_critique(con, "d0928_001", "human", ["tone"], "m", 2, 0, "g", "c1", 1) == 1
    assert store.add_critique(con, "d0928_001", "human", ["length"], "m", 2, 0, "g", "c1", 2) == 0
    assert json.loads(con.execute("SELECT gaps FROM critiques").fetchone()[0]) == ["tone"]
    assert store.critiqued_items(con, "g") == {"d0928_001"}
    assert store.add_reference(con, "+15550000001", 42, "ctx", "reply", "g", "r1", 1) == 1
    assert store.add_reference(con, "+15550000001", 42, "ctx", "reply2", "g", "r1", 2) == 0
    assert store.referenced_rowids(con, "g") == {42}
    assert con.execute("SELECT rated FROM reference_replies").fetchone()[0] is None


def test_runs_are_recorded():
    con = store.open_store(":memory:")
    rid = store.start_run(con, "g", "audit,gold", 10)
    store.finish_run(con, rid, 0, '{"audit": {}}', 20)
    assert con.execute("SELECT exit_code, finished_at_ms FROM runs WHERE id=?", (rid,)).fetchone() == (0, 20)
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_second_opinion_store.py tests/test_second_opinion_stats.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'second_opinion'`.

- [ ] **Step 3: Write the implementation**

```python
# scripts/second_opinion/__init__.py
"""Second-opinion lane: a different model family checks h-uman overnight.
Spec: docs/superpowers/specs/2026-09-29-second-opinion-lane-design.md"""
```

```python
# scripts/second_opinion/stats.py
"""Measurement math. A rate over an empty (or too small) denominator is
"not measured", never 0 (.claude/rules/no-number-without-a-measurement.md)."""
import math

NOT_MEASURED = "not measured"


def wilson(k, n, z=1.96):
    if n <= 0:
        return None
    p = k / n
    d = 1 + z * z / n
    center = (p + z * z / (2 * n)) / d
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return max(0.0, center - half), min(1.0, center + half)


def rate(k, n, min_n=1):
    if n <= 0 or n < min_n:
        return NOT_MEASURED
    lo, hi = wilson(k, n)
    return {"k": k, "n": n, "rate": round(k / n, 4), "ci95": [round(lo, 4), round(hi, 4)]}


def cohen_kappa(pairs):
    n = len(pairs)
    if n == 0:
        return None
    labels = {x for p in pairs for x in p}
    po = sum(1 for a, b in pairs if a == b) / n
    pe = sum((sum(1 for a, _ in pairs if a == lab) / n) *
             (sum(1 for _, b in pairs if b == lab) / n) for lab in labels)
    if pe == 1:
        return 1.0 if po == 1 else 0.0
    return (po - pe) / (1 - pe)
```

```python
# scripts/second_opinion/store.py
"""The lane's own store (spec §3). Never memory.db: the lane only reads
memory.db and chat.db. Every row carries backend + prompt_version + time."""
import json
import os
import sqlite3
import time

DEFAULT_PATH = os.path.expanduser("~/.human/second_opinion.db")

SCHEMA = """
CREATE TABLE IF NOT EXISTS audits (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  insight_id INTEGER NOT NULL,
  source TEXT NOT NULL,
  verdict TEXT NOT NULL,
  reason TEXT,
  unparseable INTEGER NOT NULL DEFAULT 0,
  backend TEXT NOT NULL,
  prompt_version TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL,
  UNIQUE(insight_id, backend)
);
CREATE TABLE IF NOT EXISTS critiques (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  item_id TEXT NOT NULL,
  weak_source TEXT NOT NULL,
  gaps TEXT NOT NULL,
  missing TEXT,
  severity INTEGER,
  unparseable INTEGER NOT NULL DEFAULT 0,
  backend TEXT NOT NULL,
  prompt_version TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL,
  UNIQUE(item_id, backend)
);
CREATE TABLE IF NOT EXISTS reference_replies (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  contact_id TEXT NOT NULL,
  chat_rowid INTEGER NOT NULL,
  context TEXT NOT NULL,
  reply TEXT NOT NULL,
  rated INTEGER,
  backend TEXT NOT NULL,
  prompt_version TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL,
  UNIQUE(chat_rowid, backend)
);
CREATE TABLE IF NOT EXISTS runs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  started_at_ms INTEGER NOT NULL,
  finished_at_ms INTEGER,
  backend TEXT NOT NULL,
  jobs TEXT NOT NULL,
  exit_code INTEGER,
  manifest TEXT
);
"""


def open_store(path=DEFAULT_PATH):
    if path != ":memory:":
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    con = sqlite3.connect(path)
    con.executescript(SCHEMA)
    if path != ":memory:":
        os.chmod(path, 0o600)
    return con


def now_ms():
    return int(time.time() * 1000)


def _insert(con, sql, args):
    cur = con.execute(sql, args)
    con.commit()
    return cur.rowcount


def add_audit(con, insight_id, source, verdict, reason, unparseable, backend, prompt_version,
              at_ms):
    return _insert(con, "INSERT OR IGNORE INTO audits (insight_id, source, verdict, reason,"
                        " unparseable, backend, prompt_version, created_at_ms)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                   (insight_id, source, verdict, reason, int(unparseable), backend,
                    prompt_version, at_ms))


def audited_ids(con, backend):
    return {r[0] for r in con.execute("SELECT insight_id FROM audits WHERE backend = ?",
                                      (backend,))}


def add_critique(con, item_id, weak_source, gaps, missing, severity, unparseable, backend,
                 prompt_version, at_ms):
    return _insert(con, "INSERT OR IGNORE INTO critiques (item_id, weak_source, gaps, missing,"
                        " severity, unparseable, backend, prompt_version, created_at_ms)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                   (item_id, weak_source, json.dumps(list(gaps)), missing, severity,
                    int(unparseable), backend, prompt_version, at_ms))


def critiqued_items(con, backend):
    return {r[0] for r in con.execute("SELECT item_id FROM critiques WHERE backend = ?",
                                      (backend,))}


def add_reference(con, contact_id, chat_rowid, context, reply, backend, prompt_version, at_ms):
    return _insert(con, "INSERT OR IGNORE INTO reference_replies (contact_id, chat_rowid,"
                        " context, reply, backend, prompt_version, created_at_ms)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?)",
                   (contact_id, chat_rowid, context, reply, backend, prompt_version, at_ms))


def referenced_rowids(con, backend):
    return {r[0] for r in con.execute("SELECT chat_rowid FROM reference_replies"
                                      " WHERE backend = ?", (backend,))}


def start_run(con, backend, jobs, at_ms):
    cur = con.execute("INSERT INTO runs (started_at_ms, backend, jobs) VALUES (?, ?, ?)",
                      (at_ms, backend, jobs))
    con.commit()
    return cur.lastrowid


def finish_run(con, run_id, exit_code, manifest_json, at_ms):
    con.execute("UPDATE runs SET finished_at_ms = ?, exit_code = ?, manifest = ? WHERE id = ?",
                (at_ms, exit_code, manifest_json, run_id))
    con.commit()
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_second_opinion_store.py tests/test_second_opinion_stats.py`
Expected: `7 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/second_opinion/__init__.py scripts/second_opinion/store.py scripts/second_opinion/stats.py \
        tests/test_second_opinion_store.py tests/test_second_opinion_stats.py
git commit -m "feat(second-opinion): private store with provenance + measurement math"
```

### Task 2: Backends — local Gemma (default), a server lifecycle that always stops, and Vertex opt-in

**Files:**
- Create: `scripts/second_opinion/backend.py`
- Test: `tests/test_second_opinion_backend.py`

**Interfaces:**
- Produces:
  - `backend.BackendError(RuntimeError)`
  - `backend.GemmaBackend(base_url="http://127.0.0.1:8743", model=GEMMA_MODEL, timeout=300, post=_post_json)`, with `.name`, `.base_url`, `.model` and `.generate(system, user, max_tokens=400) -> str`
  - `backend.VertexBackend(allow=False, model="gemini-3.8-flash", project="johnb-2025", thinking_budget=0, timeout=120, post=_post_json, token=_adc_token, notice=sys.stderr)`, with `.name` and `.generate(...)`
  - `backend.serve_gemma(model=GEMMA_MODEL, port=8743, python=GEMMA_PYTHON, wait_s=600, popen=subprocess.Popen, health=None, sleep=time.sleep, clock=time.monotonic, log_path=None)`, a context manager yielding a `GemmaBackend`
  - Constants `GEMMA_MODEL`, `GEMMA_PORT`, `GEMMA_PYTHON`
- The `post(url, body, headers, timeout) -> dict` seam lets tests run without a network.

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_second_opinion_backend.py
"""Local by default; Vertex only with allow; the Gemma server always stops."""
import io
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import backend as be  # noqa: E402


def test_gemma_refuses_non_loopback_urls():
    for url in ("http://10.0.0.5:8743", "https://api.example.com", "http://gemma.local:8743"):
        with pytest.raises(be.BackendError):
            be.GemmaBackend(base_url=url)
    assert be.GemmaBackend().name.endswith("@local")


def test_gemma_folds_system_into_user_and_reads_content():
    seen = {}

    def post(url, body, headers, timeout):
        seen.update(url=url, body=body)
        return {"choices": [{"message": {"content": "supported\nok"}}]}

    g = be.GemmaBackend(post=post)
    assert g.generate("SYS", "USER", max_tokens=50) == "supported\nok"
    assert seen["url"] == "http://127.0.0.1:8743/v1/chat/completions"
    assert seen["body"]["messages"] == [{"role": "user", "content": "SYS\n\nUSER"}]
    assert seen["body"]["temperature"] == 0.0 and seen["body"]["max_tokens"] == 50


def test_gemma_bad_response_shape_raises():
    g = be.GemmaBackend(post=lambda *a: {"error": "x"})
    with pytest.raises(be.BackendError):
        g.generate("s", "u")


def test_vertex_is_opt_in_and_announces_itself():
    with pytest.raises(be.BackendError):
        be.VertexBackend()
    notice = io.StringIO()
    seen = {}

    def post(url, body, headers, timeout):
        seen.update(url=url, body=body, headers=headers)
        return {"candidates": [{"content": {"parts": [{"text": "unsupported"}, {"text": "\nr"}]}}]}

    v = be.VertexBackend(allow=True, post=post, token=lambda: "tok", notice=notice)
    assert "leave this Mac" in notice.getvalue()
    assert v.generate("S", "U", max_tokens=80) == "unsupported\nr"
    assert "gemini-3.8-flash:generateContent" in seen["url"] and "johnb-2025" in seen["url"]
    gc = seen["body"]["generationConfig"]
    assert gc["thinkingConfig"]["thinkingBudget"] == 0 and gc["maxOutputTokens"] == 80
    assert seen["headers"]["Authorization"] == "Bearer tok"
    assert v.name == "gemini-3.8-flash@vertex"


class FakeProc:
    def __init__(self, exit_early=False):
        self.returncode = 1 if exit_early else None
        self.terminated = False

    def poll(self):
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = 0

    def wait(self, timeout=None):
        return self.returncode

    def kill(self):
        self.returncode = -9


def test_serve_gemma_yields_after_health_and_always_stops():
    proc = FakeProc()
    states = iter([False, False, True])  # pre-check: port free; then healthy on 2nd poll
    with be.serve_gemma(popen=lambda *a, **k: proc, health=lambda: next(states),
                        sleep=lambda s: None) as g:
        assert isinstance(g, be.GemmaBackend)
    assert proc.terminated


def test_serve_gemma_stops_server_when_body_raises():
    proc = FakeProc()
    states = iter([False, True])
    with pytest.raises(ValueError):
        with be.serve_gemma(popen=lambda *a, **k: proc, health=lambda: next(states),
                            sleep=lambda s: None):
            raise ValueError("job blew up")
    assert proc.terminated


def test_serve_gemma_refuses_a_port_someone_else_is_serving():
    started = []
    with pytest.raises(be.BackendError):
        with be.serve_gemma(popen=lambda *a, **k: started.append(1), health=lambda: True):
            pass
    assert started == []


def test_serve_gemma_times_out_and_stops():
    proc = FakeProc()
    t = iter([0, 0, 700])
    with pytest.raises(be.BackendError):
        with be.serve_gemma(popen=lambda *a, **k: proc, health=lambda: False,
                            sleep=lambda s: None, clock=lambda: next(t), wait_s=600):
            pass
    assert proc.terminated


def test_serve_gemma_reports_early_exit():
    proc = FakeProc(exit_early=True)
    states = iter([False, False])
    with pytest.raises(be.BackendError):
        with be.serve_gemma(popen=lambda *a, **k: proc, health=lambda: next(states),
                            sleep=lambda s: None):
            pass
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_second_opinion_backend.py`
Expected: FAIL with `ImportError: cannot import name 'backend'`.

- [ ] **Step 3: Write the implementation**

```python
# scripts/second_opinion/backend.py
"""Model access for the second-opinion lane (spec §3, §5).

Local Gemma is the default and never leaves the Mac. Vertex is opt-in only:
every job here reads real message text, and the blind-A/B rule is "never send
real messages to a cloud judge" unless Seth explicitly asks (--backend vertex).
"""
import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from contextlib import contextmanager

GEMMA_MODEL = "mlx-community/gemma-4-31b-it-4bit"
GEMMA_PORT = 8743
GEMMA_PYTHON = os.path.expanduser("~/Documents/gemma-realtime-1/.venv312/bin/python")
VERTEX_MODEL = "gemini-3.8-flash"
VERTEX_PROJECT = "johnb-2025"
ADC_PATH = os.path.expanduser("~/.config/gcloud/application_default_credentials.json")
LOOPBACK = {"127.0.0.1", "localhost", "::1"}


class BackendError(RuntimeError):
    pass


def _post_json(url, body, headers, timeout):
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", **headers})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


class GemmaBackend:
    def __init__(self, base_url=f"http://127.0.0.1:{GEMMA_PORT}", model=GEMMA_MODEL, timeout=300,
                 post=_post_json):
        if urllib.parse.urlparse(base_url).hostname not in LOOPBACK:
            raise BackendError(f"refusing non-local Gemma URL: {base_url}")
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout = timeout
        self._post = post
        self.name = f"{model.rsplit('/', 1)[-1]}@local"

    def generate(self, system, user, max_tokens=400):
        # Gemma chat templates have no system role: fold it into the user turn.
        body = {"model": self.model, "max_tokens": max_tokens, "temperature": 0.0,
                "messages": [{"role": "user", "content": f"{system}\n\n{user}"}]}
        d = self._post(self.base_url + "/v1/chat/completions", body, {}, self.timeout)
        try:
            return d["choices"][0]["message"]["content"] or ""
        except (KeyError, IndexError, TypeError) as e:
            raise BackendError(f"unexpected Gemma response shape: {e}") from e


def _adc_token(creds_path=ADC_PATH):
    if not os.path.exists(creds_path):
        raise BackendError("no ADC credentials; run `gcloud auth application-default login`")
    with open(creds_path) as f:
        creds = json.load(f)
    payload = urllib.parse.urlencode({
        "client_id": creds["client_id"], "client_secret": creds["client_secret"],
        "refresh_token": creds["refresh_token"], "grant_type": "refresh_token"}).encode()
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=payload,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())["access_token"]


class VertexBackend:
    def __init__(self, allow=False, model=VERTEX_MODEL, project=VERTEX_PROJECT, thinking_budget=0,
                 timeout=120, post=_post_json, token=_adc_token, notice=sys.stderr):
        if not allow:
            raise BackendError("the Vertex backend is opt-in: pass --backend vertex explicitly")
        print(f"second-opinion: backend=vertex; message excerpts will leave this Mac "
              f"(Google Cloud project {project})", file=notice)
        self.model = model
        self.project = project
        self.thinking_budget = thinking_budget
        self.timeout = timeout
        self._post = post
        self._token = token
        self.name = f"{model}@vertex"

    def generate(self, system, user, max_tokens=400):
        url = (f"https://aiplatform.googleapis.com/v1/projects/{self.project}/locations/global/"
               f"publishers/google/models/{self.model}:generateContent")
        body = {"systemInstruction": {"parts": [{"text": system}]},
                "contents": [{"role": "user", "parts": [{"text": user}]}],
                "generationConfig": {"temperature": 0.0, "maxOutputTokens": max_tokens,
                                     "thinkingConfig": {"thinkingBudget": self.thinking_budget}}}
        d = self._post(url, body, {"Authorization": f"Bearer {self._token()}"}, self.timeout)
        try:
            return "".join(p.get("text", "") for p in d["candidates"][0]["content"]["parts"])
        except (KeyError, IndexError, TypeError) as e:
            raise BackendError(f"unexpected Vertex response shape: {e}") from e


def _healthy(url):
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return r.status == 200
    except Exception:
        return False


@contextmanager
def serve_gemma(model=GEMMA_MODEL, port=GEMMA_PORT, python=GEMMA_PYTHON, wait_s=600,
                popen=subprocess.Popen, health=None, sleep=time.sleep, clock=time.monotonic,
                log_path=None):
    """Start mlx_lm.server on 127.0.0.1:<port>, yield a GemmaBackend once healthy, and
    always stop it. Refuses a port that is already serving: it may be another
    session's spare server (.claude/rules/session-worktree-isolation.md)."""
    if health is None:
        health = lambda: _healthy(f"http://127.0.0.1:{port}/health")  # noqa: E731
    if health():
        raise BackendError(f"port {port} is already serving; refusing to share or kill it")
    cmd = [python, "-m", "mlx_lm.server", "--model", model, "--host", "127.0.0.1",
           "--port", str(port)]
    if log_path:
        os.makedirs(os.path.dirname(log_path) or ".", exist_ok=True)
    log = open(log_path, "ab") if log_path else subprocess.DEVNULL
    proc = popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = clock() + wait_s
        while not health():
            if proc.poll() is not None:
                raise BackendError(f"Gemma server exited before becoming healthy "
                                   f"(rc={proc.returncode})")
            if clock() >= deadline:
                raise BackendError(f"Gemma server not healthy after {wait_s}s")
            sleep(2)
        yield GemmaBackend(f"http://127.0.0.1:{port}", model)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if log_path:
            log.close()
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_second_opinion_backend.py`
Expected: `9 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/second_opinion/backend.py tests/test_second_opinion_backend.py
git commit -m "feat(second-opinion): local Gemma backend + always-stop server lifecycle; Vertex opt-in"
```

### Task 3: Verifier audit

**Files:**
- Create: `scripts/second_opinion/audit.py`
- Test: `tests/test_second_opinion_audit.py`

**Interfaces:**
- Consumes: `store.*` and `stats.rate` (Task 1); any backend with `.name` and `.generate(system, user, max_tokens)` (Task 2).
- Produces:
  - `audit.PROMPT_VERSION = "audit-v1"`
  - `audit.source_kind(source) -> "wide" | "persona"`
  - `audit.resolve_evidence(evidence_ids, mem, chat) -> list[str]` (empty means unresolvable)
  - `audit.parse_verdict(text) -> (verdict, reason, unparseable: bool)`
  - `audit.audit_pass(store_con, backend, mem, chat, limit, deadline=None, now=None) -> dict`, with counts `sampled, attempted, audited, supported, unsupported, unclear, unparseable, skipped_no_evidence, errors, stopped_at_deadline`. An unresolvable note is stored once with verdict `no_evidence`, so it is never re-sampled. That verdict is outside every rate.
  - `audit.audit_report(store_con, since_ms=0) -> dict`, keyed `persona`, `wide` and `all`, each with `supported, unsupported, unclear, disagreement`
- `mem` is a read-only memory.db connection (tables `contact_insights`, `messages`). `chat` is a read-only chat.db connection (table `message`) or `None`.

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_second_opinion_audit.py
"""Verifier audit (spec §4.1): only cited evidence, never guessed verdicts."""
import datetime as dt
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import audit, stats, store  # noqa: E402


def mem_db():
    m = sqlite3.connect(":memory:")
    m.executescript("""
      CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, content TEXT,
                             created_at TEXT);
      CREATE TABLE contact_insights (id INTEGER PRIMARY KEY AUTOINCREMENT, contact_id TEXT,
        kind TEXT, insight TEXT, confidence REAL, as_of_ms INTEGER, source TEXT,
        created_at_ms INTEGER, retired_at_ms INTEGER DEFAULT 0, evidence_ids TEXT,
        superseded_by_id INTEGER DEFAULT 0);
      INSERT INTO messages VALUES (1, '+1a', 'user', 'priya surgery is tuesday', '');
      INSERT INTO contact_insights (contact_id, insight, source, created_at_ms, evidence_ids)
        VALUES ('+1a', 'Priya surgery tuesday', 'extractor:v2:k3:a3', 3, '[1]'),
               ('+1b', 'Dana moving to Austin', 'curator_wide:extractor:v2:k3:a3', 2, '["chat:7"]'),
               ('+1c', 'no evidence note', 'extractor:v1', 1, NULL),
               ('+1d', 'dangling', 'extractor:v1', 0, '[999]'),
               ('+1e', 'retired', 'extractor:v1', 5, '[1]');
      UPDATE contact_insights SET retired_at_ms = 9 WHERE insight = 'retired';
    """)
    return m


def chat_db():
    c = sqlite3.connect(":memory:")
    c.executescript("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT,"
                    " attributedBody BLOB);"
                    "INSERT INTO message VALUES (7, 'dana is moving to austin in may', NULL);")
    return c


class Fake:
    name = "fake@local"

    def __init__(self, outputs):
        self.outputs = list(outputs)
        self.calls = 0

    def generate(self, system, user, max_tokens=400):
        self.calls += 1
        out = self.outputs.pop(0)
        if isinstance(out, Exception):
            raise out
        return out


def test_resolve_evidence_persona_wide_and_unresolvable():
    m, c = mem_db(), chat_db()
    assert audit.resolve_evidence("[1]", m, c) == ["priya surgery is tuesday"]
    assert audit.resolve_evidence('["chat:7"]', m, c) == ["dana is moving to austin in may"]
    for bad in (None, "", "not json", '{"a": 1}', "[999]", '["chat:x"]', '["chat:8"]'):
        assert audit.resolve_evidence(bad, m, c) == [], bad
    assert audit.resolve_evidence('["chat:7"]', m, None) == []


def test_parse_verdict_never_guesses():
    assert audit.parse_verdict("Supported.\nthey say so") == ("supported", "they say so", False)
    assert audit.parse_verdict("**unsupported**") == ("unsupported", "", False)
    assert audit.parse_verdict("Sure! Here is my analysis: supported")[0] == "unclear"
    assert audit.parse_verdict("Sure! Here is my analysis")[2] is True
    assert audit.parse_verdict("") == ("unclear", "", True)


def test_audit_pass_counts_skips_and_dedupes():
    m, c, s = mem_db(), chat_db(), store.open_store(":memory:")
    b = Fake(["supported\nok", "unsupported\nno mention of Austin"])
    counts = audit.audit_pass(s, b, m, c, limit=10)
    assert counts["sampled"] == 4 and counts["skipped_no_evidence"] == 2
    assert counts["audited"] == 2 and counts["supported"] == 1 and counts["unsupported"] == 1
    assert b.calls == 2
    rows = dict(s.execute("SELECT source, verdict FROM audits").fetchall())
    assert rows == {"persona": "supported", "wide": "unsupported"}
    again = audit.audit_pass(s, Fake([]), m, c, limit=10)  # second night: nothing new to judge
    assert again["sampled"] == 0 and again["audited"] == 0  # unresolvable ones recorded once
    assert audit.audit_report(s)["all"]["disagreement"]["n"] == 2  # no_evidence is not a verdict


def test_audit_pass_counts_errors_and_unparseable():
    m, c, s = mem_db(), chat_db(), store.open_store(":memory:")
    counts = audit.audit_pass(s, Fake([TimeoutError("slow"), "I think maybe?"]), m, c, limit=10)
    assert counts["errors"] == 1 and counts["unparseable"] == 1 and counts["unclear"] == 1
    assert counts["attempted"] == 2


def test_audit_pass_stops_at_deadline():
    m, c, s = mem_db(), chat_db(), store.open_store(":memory:")
    now = dt.datetime(2026, 9, 29, 8, 0, tzinfo=dt.timezone.utc)
    counts = audit.audit_pass(s, Fake([]), m, c, limit=10, deadline=now, now=lambda: now)
    assert counts["stopped_at_deadline"] == 1 and counts["audited"] == 0


def test_audit_report_disagreement_by_source_and_not_measured():
    s = store.open_store(":memory:")
    assert audit.audit_report(s)["all"]["disagreement"] == stats.NOT_MEASURED
    for i, (src, v) in enumerate([("wide", "unsupported"), ("wide", "supported"),
                                  ("persona", "supported"), ("persona", "unclear")]):
        store.add_audit(s, i, src, v, "", 0, "g", "audit-v1", 10)
    rep = audit.audit_report(s)
    assert rep["wide"]["disagreement"]["rate"] == 0.5
    assert rep["persona"]["disagreement"]["rate"] == 0.0 and rep["persona"]["unclear"] == 1
    assert rep["all"]["disagreement"]["n"] == 3
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_second_opinion_audit.py`
Expected: FAIL with `ImportError: cannot import name 'audit'`.

- [ ] **Step 3: Write the implementation**

```python
# scripts/second_opinion/audit.py
"""Verifier audit (spec §4.1): a different model family re-checks kept curator
notes against ONLY the messages they cite. Its verdict is a second opinion,
not truth; audit_sheet/audit_score turn it into a real error rate."""
import datetime as dt
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "blind_ab"))
from imessage_text import msg_text  # noqa: E402

from . import stats, store  # noqa: E402

PROMPT_VERSION = "audit-v1"
VERDICTS = ("supported", "unsupported", "unclear")
SYSTEM = ("You check whether a short memory note about a person is supported by the text "
          "messages it cites. On the first line answer with exactly one word: supported, "
          "unsupported, or unclear. supported = the messages state it. unsupported = the "
          "messages do not state it, or contradict it. unclear = the messages are ambiguous. "
          "On the second line give one short reason.")


def source_kind(source):
    return "wide" if (source or "").startswith("curator_wide") else "persona"


def resolve_evidence(evidence_ids, mem, chat):
    """Cited message texts, or [] if ANY citation can't be resolved: a note is
    never judged on partial evidence."""
    try:
        ids = json.loads(evidence_ids) if evidence_ids else []
    except ValueError:
        return []
    if not isinstance(ids, list) or not ids:
        return []
    texts = []
    try:
        for e in ids:
            s = str(e)
            if s.startswith("chat:"):
                if chat is None:
                    return []
                row = chat.execute("SELECT text, attributedBody FROM message WHERE ROWID = ?",
                                   (int(s[5:]),)).fetchone()
                t = msg_text(row[0], row[1]) if row else None
            else:
                row = mem.execute("SELECT content FROM messages WHERE id = ?",
                                  (int(s),)).fetchone()
                t = row[0] if row else None
            if not t or not t.strip():
                return []
            texts.append(t.strip().replace("\n", " ")[:300])
    except (ValueError, TypeError):
        return []
    return texts


def parse_verdict(text):
    lines = [ln.strip() for ln in (text or "").strip().splitlines() if ln.strip()]
    if not lines:
        return "unclear", "", True
    first = re.sub(r"[^a-z]", "", lines[0].lower())
    if first in VERDICTS:
        return first, (lines[1] if len(lines) > 1 else "")[:200], False
    return "unclear", "", True


def _utcnow():
    return dt.datetime.now(dt.timezone.utc)


def audit_pass(store_con, backend, mem, chat, limit, deadline=None, now=None):
    now = now or _utcnow
    c = {k: 0 for k in ("sampled", "attempted", "audited", "supported", "unsupported", "unclear",
                        "unparseable", "skipped_no_evidence", "errors", "stopped_at_deadline")}
    done = store.audited_ids(store_con, backend.name)
    rows = mem.execute("SELECT id, insight, source, evidence_ids FROM contact_insights"
                       " WHERE retired_at_ms = 0 ORDER BY created_at_ms DESC, id DESC").fetchall()
    for iid, note, source, ev in rows:
        if c["sampled"] >= limit:
            break
        if iid in done:
            continue
        if deadline is not None and now() >= deadline:
            c["stopped_at_deadline"] = 1
            break
        c["sampled"] += 1
        texts = resolve_evidence(ev, mem, chat)
        if not texts:
            # Recorded once, so an unresolvable note never re-consumes the nightly
            # limit; 'no_evidence' is outside every rate (audit_report reads only
            # supported/unsupported/unclear).
            store.add_audit(store_con, iid, source_kind(source), "no_evidence", "", False,
                            backend.name, PROMPT_VERSION, store.now_ms())
            c["skipped_no_evidence"] += 1
            continue
        c["attempted"] += 1
        user = "Note: " + note + "\n\nCited messages:\n" + "\n".join(f"- {t}" for t in texts)
        try:
            raw = backend.generate(SYSTEM, user, max_tokens=120)
        except Exception:
            c["errors"] += 1
            continue
        verdict, reason, bad = parse_verdict(raw)
        store.add_audit(store_con, iid, source_kind(source), verdict, reason, bad, backend.name,
                        PROMPT_VERSION, store.now_ms())
        c["audited"] += 1
        c[verdict] += 1
        c["unparseable"] += int(bad)
    return c


def audit_report(store_con, since_ms=0):
    out = {}
    for src in ("persona", "wide", "all"):
        q = "SELECT verdict, COUNT(*) FROM audits WHERE created_at_ms >= ?"
        args = [since_ms]
        if src != "all":
            q += " AND source = ?"
            args.append(src)
        n = dict(store_con.execute(q + " GROUP BY verdict", args).fetchall())
        sup, uns, unc = n.get("supported", 0), n.get("unsupported", 0), n.get("unclear", 0)
        out[src] = {"supported": sup, "unsupported": uns, "unclear": unc,
                    "disagreement": stats.rate(uns, sup + uns)}
    return out
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_second_opinion_audit.py`
Expected: `6 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/second_opinion/audit.py tests/test_second_opinion_audit.py
git commit -m "feat(second-opinion): verifier audit against cited evidence only"
```

### Task 4: One-time human calibration of the audit

**Files:**
- Create: `scripts/second_opinion/audit_sheet.py`, `scripts/second_opinion/audit_score.py`
- Test: `tests/test_second_opinion_calibration.py`

**Interfaces:**
- Consumes: `audit.resolve_evidence`, `stats.wilson`, `stats.NOT_MEASURED`, and the store's `audits` table.
- Produces:
  - `audit_sheet.write_check_sheet(store_con, mem, chat, out_csv, key_json, n_unsupported=20, n_supported=10, seed=0) -> int`. Returns 0 on success, or 2 when there are too few audits (it then writes nothing). CSV columns: `row, note, cited_messages, supported`. The key JSON maps `{row: {"insight_id": int, "gemma": verdict}}`.
  - `audit_score.score_check(sheet_csv, key_json, disagreement_rate) -> dict | str`. Returns `{"ppv","false_omission","estimated_wrong_accept_rate","ci95":[lo,hi],"n_labeled"}` or `NOT_MEASURED` if any row is unlabeled.
  - Both modules have a `main(argv)` CLI.

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_second_opinion_calibration.py
"""Turn Gemma's disagreement rate into a real wrong-accept estimate (spec §4.1)."""
import csv
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import audit_score, audit_sheet, stats, store  # noqa: E402


def dbs(n):
    m = sqlite3.connect(":memory:")
    m.executescript("CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT,"
                    " content TEXT, created_at TEXT);"
                    "CREATE TABLE contact_insights (id INTEGER PRIMARY KEY, contact_id TEXT,"
                    " insight TEXT, evidence_ids TEXT, retired_at_ms INTEGER DEFAULT 0,"
                    " created_at_ms INTEGER DEFAULT 0, source TEXT);")
    for i in range(n):
        m.execute("INSERT INTO messages VALUES (?, 'c', 'user', ?, '')", (i, f"msg {i}"))
        m.execute("INSERT INTO contact_insights (id, contact_id, insight, evidence_ids)"
                  " VALUES (?, 'c', ?, ?)", (i, f"note {i}", json.dumps([i])))
    return m


def test_sheet_refuses_until_enough_audits_then_hides_the_verdict(tmp_path):
    m, s = dbs(40), store.open_store(":memory:")
    out, key = tmp_path / "check.csv", tmp_path / "key.json"
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key)) == 2
    assert not out.exists() and not key.exists()
    for i in range(25):
        store.add_audit(s, i, "persona", "unsupported", "", 0, "g", "audit-v1", 1)
    for i in range(25, 40):
        store.add_audit(s, i, "persona", "supported", "", 0, "g", "audit-v1", 1)
    assert audit_sheet.write_check_sheet(s, m, None, str(out), str(key)) == 0
    rows = list(csv.DictReader(open(out)))
    assert len(rows) == 30 and set(rows[0]) == {"row", "note", "cited_messages", "supported"}
    assert all(r["supported"] == "" for r in rows)
    assert "unsupported" not in out.read_text()
    k = json.loads(key.read_text())
    assert sum(1 for v in k.values() if v["gemma"] == "unsupported") == 20


def write_labeled(tmp_path, labels):
    """labels: list of (gemma_verdict, human_says_supported 'y'|'n'|'')."""
    sheet, key = tmp_path / "c.csv", tmp_path / "k.json"
    with open(sheet, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["row", "note", "cited_messages", "supported"])
        w.writeheader()
        for i, (_, lab) in enumerate(labels):
            w.writerow({"row": str(i), "note": "n", "cited_messages": "m", "supported": lab})
    key.write_text(json.dumps({str(i): {"insight_id": i, "gemma": g}
                               for i, (g, _) in enumerate(labels)}))
    return str(sheet), str(key)


def test_score_combines_disagreement_with_precision(tmp_path):
    labels = ([("unsupported", "n")] * 15 + [("unsupported", "y")] * 5 +
              [("supported", "y")] * 9 + [("supported", "n")] * 1)
    sheet, key = write_labeled(tmp_path, labels)
    r = audit_score.score_check(sheet, key, disagreement_rate=0.2)
    assert r["ppv"] == 0.75 and r["false_omission"] == 0.1
    assert abs(r["estimated_wrong_accept_rate"] - (0.2 * 0.75 + 0.8 * 0.1)) < 1e-9
    lo, hi = r["ci95"]
    assert lo < r["estimated_wrong_accept_rate"] < hi and r["n_labeled"] == 30


def test_score_is_not_measured_until_every_row_is_labeled(tmp_path):
    sheet, key = write_labeled(tmp_path, [("unsupported", "n"), ("supported", "")])
    assert audit_score.score_check(sheet, key, disagreement_rate=0.2) == stats.NOT_MEASURED
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_second_opinion_calibration.py`
Expected: FAIL with `ImportError`.

- [ ] **Step 3: Write the implementation**

```python
# scripts/second_opinion/audit_sheet.py
"""Build the one-time 30-row check sheet (spec §4.1). The verdict is hidden;
the key stays private next to the sheet."""
import argparse
import csv
import json
import os
import random
import sqlite3
import sys

from . import audit, store


def _pick(store_con, verdict, n, rng):
    ids = [r[0] for r in store_con.execute(
        "SELECT DISTINCT insight_id FROM audits WHERE verdict = ?", (verdict,))]
    rng.shuffle(ids)
    return ids[:n] if len(ids) >= n else None


def write_check_sheet(store_con, mem, chat, out_csv, key_json, n_unsupported=20, n_supported=10,
                      seed=0):
    rng = random.Random(seed)
    uns = _pick(store_con, "unsupported", n_unsupported, rng)
    sup = _pick(store_con, "supported", n_supported, rng)
    if uns is None or sup is None:
        print(f"refusing: need {n_unsupported} unsupported and {n_supported} supported audits",
              file=sys.stderr)
        return 2
    picks = [(i, "unsupported") for i in uns] + [(i, "supported") for i in sup]
    rng.shuffle(picks)
    rows, key = [], {}
    for n, (iid, verdict) in enumerate(picks):
        rec = mem.execute("SELECT insight, evidence_ids FROM contact_insights WHERE id = ?",
                          (iid,)).fetchone()
        texts = audit.resolve_evidence(rec[1], mem, chat) if rec else []
        rows.append({"row": str(n), "note": rec[0] if rec else "",
                     "cited_messages": " | ".join(texts), "supported": ""})
        key[str(n)] = {"insight_id": iid, "gemma": verdict}
    with open(out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["row", "note", "cited_messages", "supported"])
        w.writeheader()
        w.writerows(rows)
    with open(key_json, "w") as f:
        json.dump(key, f, indent=1)
    os.chmod(out_csv, 0o600)
    os.chmod(key_json, 0o600)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    ap.add_argument("--mem-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--out", default=os.path.expanduser("~/.human/second_opinion/audit_check.csv"))
    a = ap.parse_args(argv)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    mem = sqlite3.connect(f"file:{a.mem_db}?mode=ro", uri=True)
    chat = sqlite3.connect(f"file:{a.chat_db}?mode=ro", uri=True)
    return write_check_sheet(store.open_store(a.store), mem, chat, a.out,
                             a.out.replace(".csv", ".key.json"))


if __name__ == "__main__":
    sys.exit(main())
```

```python
# scripts/second_opinion/audit_score.py
"""Score Seth's one-time check (spec §4.1). The curator's wrong-accept rate is
estimated as d*PPV + (1-d)*FOR: d = Gemma's disagreement rate on kept notes,
PPV = share of Gemma-'unsupported' notes Seth also calls unsupported, FOR =
share of Gemma-'supported' notes Seth calls unsupported. The interval combines
the Wilson bounds of PPV and FOR (conservative)."""
import argparse
import csv
import json
import sys

from . import audit, stats, store


def score_check(sheet_csv, key_json, disagreement_rate):
    key = json.load(open(key_json))
    rows = list(csv.DictReader(open(sheet_csv)))
    labels = {r["row"]: (r.get("supported") or "").strip().lower()[:1] for r in rows}
    if not rows or any(labels.get(k) not in ("y", "n") for k in key):
        return stats.NOT_MEASURED
    u = [labels[k] for k, v in key.items() if v["gemma"] == "unsupported"]
    s = [labels[k] for k, v in key.items() if v["gemma"] == "supported"]
    if not u or not s:
        return stats.NOT_MEASURED
    ppv_k, for_k = u.count("n"), s.count("n")
    ppv, fo = ppv_k / len(u), for_k / len(s)
    d = disagreement_rate
    p_lo, p_hi = stats.wilson(ppv_k, len(u))
    f_lo, f_hi = stats.wilson(for_k, len(s))
    return {"ppv": round(ppv, 4), "false_omission": round(fo, 4),
            "estimated_wrong_accept_rate": d * ppv + (1 - d) * fo,
            "ci95": [round(d * p_lo + (1 - d) * f_lo, 4), round(d * p_hi + (1 - d) * f_hi, 4)],
            "n_labeled": len(u) + len(s), "disagreement_rate": d}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("sheet")
    ap.add_argument("--key", required=True)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    a = ap.parse_args(argv)
    dis = audit.audit_report(store.open_store(a.store))["all"]["disagreement"]
    if dis == stats.NOT_MEASURED:
        print("refusing: no audits yet", file=sys.stderr)
        return 2
    r = score_check(a.sheet, a.key, dis["rate"])
    print(json.dumps(r, indent=1))
    return 0 if r != stats.NOT_MEASURED else 2


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_second_opinion_calibration.py`
Expected: `3 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/second_opinion/audit_sheet.py scripts/second_opinion/audit_score.py \
        tests/test_second_opinion_calibration.py
git commit -m "feat(second-opinion): one-time human check turns disagreement into a wrong-accept rate"
```

### Task 5: Judge calibration

**Files:**
- Create: `scripts/second_opinion/judge.py`
- Test: `tests/test_second_opinion_judge.py`

**Interfaces:**
- Consumes: `stats.cohen_kappa`, `stats.NOT_MEASURED`; `scripts/blind_ab/synthetic_judge.py` (CLI `sheet --out OUT --endpoint URL --model M`); and `scripts/blind_ab/score.py` (CLI `sheets --key KEY --rater synthetic --json-out OUT`).
- Produces:
  - `judge.latest_run_dir(root) -> str | None`: the newest directory containing both `rating_sheet.csv` and `answer_key.json`
  - `judge.human_choices(run_dir) -> dict[id, "A"|"B"]`: filled rows from CSVs without a non-empty `judge_model` column
  - `judge.calibration(human, judged) -> dict`, containing `shared, agreement, kappa`; `kappa` is `NOT_MEASURED` when `shared < 20`
  - `judge.judge_pass(backend, run_dir, out_dir, run=subprocess.run) -> dict`, which needs a backend with `.base_url` and `.model` (Gemma). With any other backend it returns `{"skipped": "backend"}`.

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_second_opinion_judge.py
"""Judge calibration (spec §4.2): synthetic key only, κ needs ≥ 20 shared items."""
import csv
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import judge, stats  # noqa: E402

COLS = ["id", "context", "option_A", "option_B", "choice", "confidence"]


def sheet(path, choices, judged=False):
    cols = COLS + (["judge_model"] if judged else [])
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for i, ch in enumerate(choices):
            row = {"id": f"x{i}", "context": "c", "option_A": "a", "option_B": "b",
                   "choice": ch, "confidence": "4"}
            if judged:
                row["judge_model"] = "gemma"
            w.writerow(row)


def test_human_choices_ignore_blank_and_judged_sheets(tmp_path):
    sheet(tmp_path / "rating_sheet.csv", ["", "", ""])            # nobody rated yet
    sheet(tmp_path / "rating_sheet_seth.csv", ["A", "", "b"])
    sheet(tmp_path / "judged.csv", ["B", "B", "B"], judged=True)
    assert judge.human_choices(str(tmp_path)) == {"x0": "A", "x2": "B"}


def test_calibration_needs_twenty_shared_items():
    few = judge.calibration({"a": "A"}, {"a": "A"})
    assert few["shared"] == 1 and few["kappa"] == stats.NOT_MEASURED
    h = {f"i{n}": ("A" if n % 2 else "B") for n in range(20)}
    r = judge.calibration(h, dict(h))
    assert r["shared"] == 20 and r["agreement"] == 1.0 and r["kappa"] == 1.0


def test_latest_run_dir_requires_sheet_and_key(tmp_path):
    (tmp_path / "old").mkdir()
    (tmp_path / "new").mkdir()
    for d in ("old", "new"):
        (tmp_path / d / "rating_sheet.csv").write_text("id\n")
    (tmp_path / "old" / "answer_key.json").write_text("{}")
    assert judge.latest_run_dir(str(tmp_path)) == str(tmp_path / "old")
    assert judge.latest_run_dir(str(tmp_path / "missing")) is None


class G:
    base_url = "http://127.0.0.1:8743"
    model = "mlx-community/gemma-4-31b-it-4bit"
    name = "gemma-4-31b-it-4bit@local"


def test_judge_pass_runs_synthetic_rater_and_calibrates(tmp_path):
    run_dir, out = tmp_path / "run", tmp_path / "out"
    run_dir.mkdir()
    sheet(run_dir / "rating_sheet.csv", [""] * 3)
    (run_dir / "answer_key.json").write_text(json.dumps({"x0": "A", "x1": "B", "x2": "A"}))
    sheet(run_dir / "rating_sheet_seth.csv", ["A", "B", "A"])
    calls = []

    class R:
        returncode = 0

    def run(cmd, **kw):
        calls.append(cmd)
        if cmd[1].endswith("synthetic_judge.py"):
            sheet(cmd[cmd.index("--out") + 1], ["A", "B", "B"], judged=True)
        else:
            Path(cmd[cmd.index("--json-out") + 1]).write_text(json.dumps({"detection": 0.33}))
        return R()

    r = judge.judge_pass(G(), str(run_dir), str(out), run=run)
    assert "--rater" in calls[1] and calls[1][calls[1].index("--rater") + 1] == "synthetic"
    assert calls[0][calls[0].index("--endpoint") + 1] == G.base_url + "/v1/chat/completions"
    assert r["calibration"]["shared"] == 3 and r["calibration"]["kappa"] == stats.NOT_MEASURED
    assert r["calibration"]["agreement"] == round(2 / 3, 4)


def test_judge_pass_skips_non_local_backends(tmp_path):
    class V:
        name = "gemini-3.8-flash@vertex"

    assert judge.judge_pass(V(), str(tmp_path), str(tmp_path)) == {"skipped": "backend"}
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_second_opinion_judge.py`
Expected: FAIL with `ImportError`.

- [ ] **Step 3: Write the implementation**

```python
# scripts/second_opinion/judge.py
"""Judge calibration (spec §4.2). Reuses synthetic_judge.py unchanged against
local Gemma, records with score.py --rater synthetic (never the human key),
and measures agreement with Seth's own ratings on the same items."""
import csv
import glob
import json
import os
import subprocess
import sys

from . import stats

SCRIPTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLIND_AB = os.path.join(SCRIPTS, "blind_ab")


def latest_run_dir(root):
    if not os.path.isdir(root):
        return None
    dirs = [d for d in glob.glob(os.path.join(root, "*"))
            if os.path.isfile(os.path.join(d, "rating_sheet.csv"))
            and os.path.isfile(os.path.join(d, "answer_key.json"))]
    return max(dirs, key=os.path.getmtime) if dirs else None


def _choices(path):
    out = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            ch = (r.get("choice") or "").strip().upper()
            if ch in ("A", "B") and r.get("id"):
                out[r["id"]] = ch
    return out


def _is_judged(path):
    with open(path, newline="") as f:
        return any((r.get("judge_model") or "").strip() for r in csv.DictReader(f))


def human_choices(run_dir):
    out = {}
    for p in sorted(glob.glob(os.path.join(run_dir, "*.csv"))):
        if not _is_judged(p):
            out.update(_choices(p))
    return out


def calibration(human, judged):
    shared = sorted(set(human) & set(judged))
    pairs = [(human[i], judged[i]) for i in shared]
    agree = round(sum(1 for a, b in pairs if a == b) / len(pairs), 4) if pairs else None
    kappa = stats.cohen_kappa(pairs) if len(pairs) >= 20 else stats.NOT_MEASURED
    return {"shared": len(pairs), "agreement": agree if pairs else stats.NOT_MEASURED,
            "kappa": round(kappa, 4) if isinstance(kappa, float) else kappa}


def judge_pass(backend, run_dir, out_dir, run=subprocess.run):
    if not hasattr(backend, "base_url"):
        return {"skipped": "backend"}
    os.makedirs(out_dir, exist_ok=True)
    judged = os.path.join(out_dir, "judged.csv")
    results = os.path.join(out_dir, "judge-results.json")
    r1 = run([sys.executable, os.path.join(BLIND_AB, "synthetic_judge.py"),
              os.path.join(run_dir, "rating_sheet.csv"), "--out", judged,
              "--endpoint", backend.base_url + "/v1/chat/completions", "--model", backend.model],
             capture_output=True, text=True)
    if r1.returncode != 0:
        raise RuntimeError("synthetic_judge.py failed")
    r2 = run([sys.executable, os.path.join(BLIND_AB, "score.py"), judged,
              "--key", os.path.join(run_dir, "answer_key.json"), "--rater", "synthetic",
              "--json-out", results], capture_output=True, text=True)
    if r2.returncode != 0:
        raise RuntimeError("score.py failed")
    return {"results": json.load(open(results)),
            "calibration": calibration(human_choices(run_dir), _choices(judged))}
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_second_opinion_judge.py`
Expected: `5 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/second_opinion/judge.py tests/test_second_opinion_judge.py
git commit -m "feat(second-opinion): weekly cross-family judge calibration (synthetic key only)"
```

### Task 6: Gold — critiques and reference replies

**Files:**
- Create: `scripts/second_opinion/gold.py`
- Test: `tests/test_second_opinion_gold.py`

**Interfaces:**
- Consumes: `store.add_critique`, `store.critiqued_items`, `store.add_reference`, `store.referenced_rowids` (Task 1); `judge.human_choices`-style CSV reading (inlined here); a backend (Task 2); `eval_conversation_quality.attribute()` output (`timelines` with message dicts `rowid, t, from_me, text`; `labeled` of `(msg, label)` pairs).
- Produces:
  - `gold.CRITIQUE_VERSION = "critique-v1"`, `gold.REFERENCE_VERSION = "reference-v1"`, `gold.GAPS`
  - `gold.weak_items(run_dir) -> list[(item_id, weak_source, triple)]`, human first (confidence ≥ 4 and the rater picked the real reply), then synthetic
  - `gold.parse_critique(text) -> (gaps, missing, severity, unparseable)`
  - `gold.unanswered_daemon_replies(att, window_h=24) -> list[(contact, msg, context_msgs)]`, newest first
  - `gold.contact_notes(mem, contact_id, wide_live) -> list[str]`
  - `gold.gold_pass(store_con, backend, run_dirs, att, mem, limit, deadline=None, now=None, wide_live=False) -> dict`, with counts `critiques, references, unparseable, errors, attempted, stopped_at_deadline`
  - `gold.gold_report(store_con, since_ms=0) -> dict`, containing `critiques, references, unparseable, gaps` (a category → count map)

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_second_opinion_gold.py
"""Critiques where Seth replied, reference replies where he didn't (spec §4.3)."""
import csv
import datetime as dt
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import gold, store  # noqa: E402

T0 = dt.datetime(2026, 9, 28, 12, 0, tzinfo=dt.timezone.utc)


def run_dir(tmp_path):
    d = tmp_path / "run"
    d.mkdir()
    (d / "triples.json").write_text(json.dumps([
        {"id": f"x{i}", "context": f"ctx {i}", "seth_reply": f"seth {i}",
         "huuman_reply": f"ai {i}"} for i in range(4)]))
    (d / "answer_key.json").write_text(json.dumps({"x0": "A", "x1": "B", "x2": "A", "x3": "B"}))
    cols = ["id", "context", "option_A", "option_B", "choice", "confidence"]
    with open(d / "rating_sheet_seth.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerow({"id": "x0", "choice": "A", "confidence": "5"})   # spotted, confident
        w.writerow({"id": "x1", "choice": "A", "confidence": "5"})   # fooled
        w.writerow({"id": "x2", "choice": "A", "confidence": "2"})   # spotted, unsure
        w.writerow({"id": "x3", "choice": "", "confidence": ""})     # not rated
    with open(d / "judged.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols + ["judge_model"])
        w.writeheader()
        w.writerow({"id": "x2", "choice": "A", "judge_model": "gemma"})
        w.writerow({"id": "x3", "choice": "B", "judge_model": "gemma"})
    return str(d)


def test_weak_items_human_first_then_synthetic(tmp_path):
    items = gold.weak_items(run_dir(tmp_path))
    assert [(i, s) for i, s, _ in items] == [("x0", "human"), ("x2", "synthetic"),
                                             ("x3", "synthetic")]
    assert items[0][2]["seth_reply"] == "seth 0"


def test_parse_critique_filters_gaps_and_never_guesses():
    ok = gold.parse_critique('ok {"gaps": ["tone", "bogus"], "missing": "the date", "severity": 7}')
    assert ok == (["tone"], "the date", 3, False)
    assert gold.parse_critique('{"gaps": [], "missing": "x"}')[0] == ["other"]
    assert gold.parse_critique("Sure, the AI reply is too long") == (["other"], "", None, True)


def msg(rowid, minutes, from_me, text):
    return {"rowid": rowid, "guid": f"g{rowid}", "t": T0 + dt.timedelta(minutes=minutes),
            "from_me": from_me, "text": text}


def att():
    them1, bot1, them2, bot2, me2 = (msg(1, 0, False, "hey"), msg(2, 1, True, "yo"),
                                     msg(3, 60, False, "dinner?"), msg(4, 61, True, "sure"),
                                     msg(5, 70, True, "7pm works"))
    return {"timelines": {"+1a": [them1, bot1, them2, bot2, me2]},
            "labeled": {"+1a": [(bot1, "huuman"), (bot2, "huuman"), (me2, "seth")]}}


def test_unanswered_daemon_replies_skip_ones_seth_followed_up():
    got = gold.unanswered_daemon_replies(att())
    assert [(c, m["rowid"]) for c, m, _ in got] == []  # seth replied within 24h after both
    a = att()
    a["labeled"]["+1a"] = a["labeled"]["+1a"][:2]      # no seth follow-up at all
    got = gold.unanswered_daemon_replies(a)
    assert [m["rowid"] for _, m, _ in got] == [4, 2]   # newest first
    assert [x["rowid"] for x in got[0][2]] == [1, 2, 3]  # context strictly before


def mem():
    m = sqlite3.connect(":memory:")
    m.executescript("CREATE TABLE contact_insights (id INTEGER PRIMARY KEY, contact_id TEXT,"
                    " insight TEXT, confidence REAL, as_of_ms INTEGER, source TEXT,"
                    " retired_at_ms INTEGER DEFAULT 0);"
                    "INSERT INTO contact_insights VALUES (1,'+1a','likes sushi',0.9,5,'extractor:v1',0),"
                    "(2,'+1a','wide note',0.9,6,'curator_wide:x',0),"
                    "(3,'+1a','low conf',0.2,7,'extractor:v1',0),"
                    "(4,'+1a','retired',0.9,8,'extractor:v1',9);")
    return m


def test_contact_notes_match_the_render_gate():
    assert gold.contact_notes(mem(), "+1a", wide_live=False) == ["likes sushi"]
    assert gold.contact_notes(mem(), "+1a", wide_live=True) == ["wide note", "likes sushi"]


class Fake:
    name = "fake@local"

    def __init__(self, outputs):
        self.outputs = list(outputs)

    def generate(self, system, user, max_tokens=400):
        out = self.outputs.pop(0)
        if isinstance(out, Exception):
            raise out
        return out


def test_gold_pass_writes_critiques_and_unrated_references_and_dedupes(tmp_path):
    s = store.open_store(":memory:")
    a = att()
    a["labeled"]["+1a"] = a["labeled"]["+1a"][:2]
    outputs = ['{"gaps":["specific_detail"],"missing":"m","severity":2}',
               "not json", TimeoutError(), "7pm? i'm in", "hey!"]
    c = gold.gold_pass(s, Fake(outputs), [run_dir(tmp_path)], a, mem(), limit=10)
    assert c["critiques"] == 2 and c["unparseable"] == 1 and c["errors"] == 1
    assert c["references"] == 2
    assert s.execute("SELECT COUNT(*) FROM reference_replies WHERE rated IS NULL").fetchone()[0] == 2
    again = gold.gold_pass(s, Fake([]), [run_dir(tmp_path)], a, mem(), limit=10)
    assert again["critiques"] == 0 and again["references"] == 0 and again["attempted"] == 1
    rep = gold.gold_report(s)
    assert rep["gaps"]["specific_detail"] == 1 and rep["gaps"]["other"] == 1
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_second_opinion_gold.py`
Expected: FAIL with `ImportError`.

- [ ] **Step 3: Write the implementation**

```python
# scripts/second_opinion/gold.py
"""Gold (spec §4.3): where Seth replied, a critique of h-uman's reply against
his real one; where h-uman replied and Seth did not, a reference reply stored
UNRATED. Nothing here is trained on or sent."""
import collections
import csv
import datetime as dt
import glob
import json
import os
import re

from . import store

CRITIQUE_VERSION = "critique-v1"
REFERENCE_VERSION = "reference-v1"
GAPS = ("specific_detail", "tone", "length", "question_vs_statement", "other")
CRITIQUE_SYSTEM = (
    "You compare two text-message replies to the same message. One is from the real person, "
    "one from an AI imitating him. Say what the AI reply is missing compared with the real "
    'one. Output only JSON: {"gaps": [...], "missing": "<one sentence>", "severity": 1-3}, '
    "where gaps are chosen from " + json.dumps(list(GAPS)) + ".")
REFERENCE_SYSTEM = (
    "Write the next text message this person would send in the conversation below. Use a "
    "detail from the notes only if it fits naturally. Reply with the message text only: one "
    "or two short lines, no quotes.")


def weak_items(run_dir):
    key = json.load(open(os.path.join(run_dir, "answer_key.json")))
    triples = {t["id"]: t for t in json.load(open(os.path.join(run_dir, "triples.json")))}
    human, synth = [], []
    for path in sorted(glob.glob(os.path.join(run_dir, "*.csv"))):
        with open(path, newline="") as f:
            rows = list(csv.DictReader(f))
        judged = any((r.get("judge_model") or "").strip() for r in rows)
        for r in rows:
            iid, ch = r.get("id"), (r.get("choice") or "").strip().upper()
            if ch not in ("A", "B") or iid not in key or iid not in triples or ch != key[iid]:
                continue
            if judged:
                synth.append(iid)
                continue
            try:
                conf = int(float(r.get("confidence") or 0))
            except ValueError:
                conf = 0
            if conf >= 4:
                human.append(iid)
    out, seen = [], set()
    for src, ids in (("human", human), ("synthetic", synth)):
        for iid in ids:
            if iid not in seen:
                seen.add(iid)
                out.append((iid, src, triples[iid]))
    return out


def parse_critique(text):
    m = re.search(r"\{[\s\S]*\}", text or "")
    try:
        obj = json.loads(m.group(0)) if m else None
    except ValueError:
        obj = None
    if not isinstance(obj, dict):
        return ["other"], "", None, True
    gaps = [g for g in (obj.get("gaps") or []) if g in GAPS] or ["other"]
    try:
        sev = max(1, min(3, int(obj.get("severity"))))
    except (TypeError, ValueError):
        sev = None
    return gaps, str(obj.get("missing") or "")[:300], sev, False


def unanswered_daemon_replies(att, window_h=24):
    out = []
    window = dt.timedelta(hours=window_h)
    for contact, labeled in att["labeled"].items():
        seth_times = [m["t"] for m, lab in labeled if lab == "seth"]
        timeline = att["timelines"].get(contact, [])
        for m, lab in labeled:
            if lab != "huuman":
                continue
            if any(m["t"] < t <= m["t"] + window for t in seth_times):
                continue
            ctx = [x for x in timeline if x["t"] < m["t"]][-6:]
            out.append((contact, m, ctx))
    out.sort(key=lambda x: x[1]["t"], reverse=True)
    return out


def contact_notes(mem, contact_id, wide_live):
    """The same selection hu_contact_insights_render makes: live, confidence >= 0.5,
    newest 8, curator_wide rows only when HU_INSIGHT_WIDE=live."""
    rows = mem.execute(
        "SELECT insight FROM contact_insights WHERE contact_id = ? AND retired_at_ms = 0"
        " AND confidence >= 0.5 AND (? OR source IS NULL OR source NOT LIKE 'curator_wide%')"
        " ORDER BY as_of_ms DESC, id DESC LIMIT 8", (contact_id, 1 if wide_live else 0))
    return [r[0] for r in rows]


def _utcnow():
    return dt.datetime.now(dt.timezone.utc)


def gold_pass(store_con, backend, run_dirs, att, mem, limit, deadline=None, now=None,
              wide_live=False):
    now = now or _utcnow
    c = {k: 0 for k in ("critiques", "references", "unparseable", "errors", "attempted",
                        "stopped_at_deadline")}

    def out_of_time():
        if deadline is not None and now() >= deadline:
            c["stopped_at_deadline"] = 1
            return True
        return False

    done = store.critiqued_items(store_con, backend.name)
    todo = [w for d in run_dirs for w in weak_items(d) if w[0] not in done]
    for iid, src, t in todo[:limit]:
        if out_of_time():
            return c
        c["attempted"] += 1
        user = (f"Message they replied to:\n{t['context']}\n\nReal reply:\n{t['seth_reply']}"
                f"\n\nAI reply:\n{t['huuman_reply']}")
        try:
            raw = backend.generate(CRITIQUE_SYSTEM, user, max_tokens=200)
        except Exception:
            c["errors"] += 1
            continue
        gaps, missing, sev, bad = parse_critique(raw)
        c["critiques"] += store.add_critique(store_con, iid, src, gaps, missing, sev, bad,
                                             backend.name, CRITIQUE_VERSION, store.now_ms())
        c["unparseable"] += int(bad)

    if att is None:
        return c
    have = store.referenced_rowids(store_con, backend.name)
    todo_r = [x for x in unanswered_daemon_replies(att) if x[1]["rowid"] not in have]
    for contact, m, ctx in todo_r[:limit]:
        if out_of_time():
            return c
        c["attempted"] += 1
        convo = "\n".join(f"{'me' if x['from_me'] else 'them'}: {x['text']}" for x in ctx)
        notes = contact_notes(mem, contact, wide_live)
        user = ("Notes about them:\n" + ("\n".join(f"- {n}" for n in notes) or "(none)")
                + f"\n\nConversation:\n{convo}\n\nNext message from me:")
        try:
            reply = backend.generate(REFERENCE_SYSTEM, user, max_tokens=120).strip()
        except Exception:
            c["errors"] += 1
            continue
        if not reply:
            c["unparseable"] += 1
            continue
        c["references"] += store.add_reference(store_con, contact, int(m["rowid"]), convo, reply,
                                               backend.name, REFERENCE_VERSION, store.now_ms())
    return c


def gold_report(store_con, since_ms=0):
    gaps = collections.Counter()
    crit = unp = 0
    for g, bad in store_con.execute("SELECT gaps, unparseable FROM critiques"
                                    " WHERE created_at_ms >= ?", (since_ms,)):
        crit += 1
        unp += bad
        gaps.update(json.loads(g))
    refs = store_con.execute("SELECT COUNT(*) FROM reference_replies WHERE created_at_ms >= ?",
                             (since_ms,)).fetchone()[0]
    return {"critiques": crit, "references": refs, "unparseable": unp, "gaps": dict(gaps)}
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_second_opinion_gold.py`
Expected: `6 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/second_opinion/gold.py tests/test_second_opinion_gold.py
git commit -m "feat(second-opinion): critiques vs Seth's real reply + unrated reference replies"
```

### Task 7: Rating and export for reference replies

**Files:**
- Create: `scripts/second_opinion/gold_rate.py`, `scripts/second_opinion/gold_export.py`
- Test: `tests/test_second_opinion_gold_io.py`

**Interfaces:**
- Consumes: the store's `reference_replies` table.
- Produces:
  - `gold_rate.write_rating_sheet(store_con, out_csv) -> int`: the number of unrated rows written, with columns `id, context, reply, good` and the file at mode 0600
  - `gold_rate.import_ratings(store_con, sheet_csv) -> dict`, containing `rated_good, rated_bad, skipped`; `y` sets `rated = 1` and `n` sets `rated = 0`; anything else is skipped
  - `gold_export.export_rated(store_con, out_csv) -> int`, which writes only `rated = 1` rows (columns `contact_id, context, reply, backend`)
  - Both modules have a `main(argv)` CLI.

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_second_opinion_gold_io.py
"""Reference replies stay out of every export until Seth rates them good."""
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import gold_export, gold_rate, store  # noqa: E402


def seeded():
    s = store.open_store(":memory:")
    for i in range(3):
        store.add_reference(s, "+1a", i, f"ctx {i}", f"reply {i}", "g@local", "reference-v1", 1)
    return s


def test_unrated_replies_are_never_exported(tmp_path):
    s = seeded()
    out = tmp_path / "export.csv"
    assert gold_export.export_rated(s, str(out)) == 0
    assert list(csv.DictReader(open(out))) == []


def test_rate_then_export_only_good(tmp_path):
    s = seeded()
    sheet = tmp_path / "rate.csv"
    assert gold_rate.write_rating_sheet(s, str(sheet)) == 3
    rows = list(csv.DictReader(open(sheet)))
    rows[0]["good"], rows[1]["good"], rows[2]["good"] = "y", "n", "maybe"
    with open(sheet, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "reply", "good"])
        w.writeheader()
        w.writerows(rows)
    assert gold_rate.import_ratings(s, str(sheet)) == {"rated_good": 1, "rated_bad": 1,
                                                       "skipped": 1}
    out = tmp_path / "export.csv"
    assert gold_export.export_rated(s, str(out)) == 1
    assert [r["reply"] for r in csv.DictReader(open(out))] == [rows[0]["reply"]]
    assert gold_rate.write_rating_sheet(s, str(sheet)) == 1  # only the skipped one remains
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_second_opinion_gold_io.py`
Expected: FAIL with `ImportError`.

- [ ] **Step 3: Write the implementation**

```python
# scripts/second_opinion/gold_rate.py
"""Rate Gemma's reference replies (spec §4.3): write a sheet of unrated rows,
then import Seth's y/n back into the store."""
import argparse
import csv
import json
import os
import sys

from . import store

FIELDS = ["id", "context", "reply", "good"]


def write_rating_sheet(store_con, out_csv):
    rows = store_con.execute("SELECT id, context, reply FROM reference_replies"
                             " WHERE rated IS NULL ORDER BY id").fetchall()
    with open(out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        for rid, ctx, reply in rows:
            w.writerow({"id": rid, "context": ctx, "reply": reply, "good": ""})
    os.chmod(out_csv, 0o600)
    return len(rows)


def import_ratings(store_con, sheet_csv):
    c = {"rated_good": 0, "rated_bad": 0, "skipped": 0}
    with open(sheet_csv, newline="") as f:
        for r in csv.DictReader(f):
            v = (r.get("good") or "").strip().lower()
            if v not in ("y", "n"):
                c["skipped"] += 1
                continue
            store_con.execute("UPDATE reference_replies SET rated = ? WHERE id = ?",
                              (1 if v == "y" else 0, int(r["id"])))
            c["rated_good" if v == "y" else "rated_bad"] += 1
    store_con.commit()
    return c


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    ap.add_argument("--write", help="write unrated rows to this CSV")
    ap.add_argument("--import", dest="imp", help="import a completed CSV")
    a = ap.parse_args(argv)
    con = store.open_store(a.store)
    if a.write:
        print(f"{write_rating_sheet(con, a.write)} unrated reference replies written")
    if a.imp:
        print(json.dumps(import_ratings(con, a.imp)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

```python
# scripts/second_opinion/gold_export.py
"""Export reference replies Seth rated good (rated = 1). Unrated or rejected
rows are never exported. No training export exists (spec §9)."""
import argparse
import csv
import os
import sys

from . import store


def export_rated(store_con, out_csv):
    rows = store_con.execute("SELECT contact_id, context, reply, backend FROM reference_replies"
                             " WHERE rated = 1 ORDER BY id").fetchall()
    with open(out_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["contact_id", "context", "reply", "backend"])
        w.writerows(rows)
    os.chmod(out_csv, 0o600)
    return len(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out")
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    a = ap.parse_args(argv)
    print(f"{export_rated(store.open_store(a.store), a.out)} rated-good replies exported")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests; they should pass**

Run: `python3 -m pytest -q tests/test_second_opinion_gold_io.py`
Expected: `2 passed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/second_opinion/gold_rate.py scripts/second_opinion/gold_export.py \
        tests/test_second_opinion_gold_io.py
git commit -m "feat(second-opinion): rate reference replies; export only rated-good rows"
```

### Task 8: The nightly runner

**Files:**
- Create: `scripts/second_opinion/run_nightly.py`
- Test: `tests/test_second_opinion_run_nightly.py`

**Interfaces:**
- Consumes: everything from Tasks 1–7; `insight_stream.resolve_deadline(hhmm, now_local) -> datetime | None` (in `scripts/`); `eval_conversation_quality.attribute(chat_path, mem_path, since)`.
- Produces: `run_nightly.main(argv=None, *, now_local=None, serve=backend.serve_gemma, attribute=None) -> int`.
  - Exit codes: 0 for done, window closed, or another run holding the lock; 2 for a refusal; 3 when every attempted item failed.
  - Flags:
    - `--jobs`: `auto` (the default) is audit and gold, plus judge and report on Sundays;
    - `--judge-run-dir`, `--blind-ab-root` (default `~/blind_ab_run`);
    - `--backend gemma|vertex`;
    - `--deadline HH:MM`;
    - `--audit-limit` (default 25), `--gold-limit` (default 10);
    - `--store`, `--manifest-dir` (default `~/.human/logs`), `--reports-dir` (default `~/.human/logs/second-opinion-reports`);
    - `--mem-db`, `--chat-db`, `--lock` (default `~/.human/second_opinion.lock`);
    - `--dry-run`.
  - The manifest is `<manifest-dir>/second-opinion-YYYYMMDD[-dryrun].json`, written atomically.

- [ ] **Step 1: Write the failing tests**

```python
# tests/test_second_opinion_run_nightly.py
"""Runner contract (spec §5, §6): refuse and write nothing; counts-only manifest."""
import contextlib
import datetime as dt
import fcntl
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import backend as be, run_nightly  # noqa: E402

LOCAL_MORNING = dt.datetime(2026, 9, 29, 7, 45).astimezone()   # a Tuesday


def mk_mem(p):
    m = sqlite3.connect(p)
    m.executescript("""
      CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, content TEXT,
                             created_at TEXT);
      CREATE TABLE contact_insights (id INTEGER PRIMARY KEY AUTOINCREMENT, contact_id TEXT,
        kind TEXT, insight TEXT, confidence REAL, as_of_ms INTEGER, source TEXT,
        created_at_ms INTEGER, retired_at_ms INTEGER DEFAULT 0, evidence_ids TEXT,
        superseded_by_id INTEGER DEFAULT 0);
      INSERT INTO messages VALUES (1, '+1a', 'user', 'priya surgery is tuesday', '');
      INSERT INTO contact_insights (contact_id, insight, confidence, source, created_at_ms,
        evidence_ids) VALUES ('+1a', 'SECRET-NOTE priya surgery', 0.9, 'extractor:v1', 1, '[1]');
    """)
    m.commit()
    m.close()


def mk_chat(p):
    c = sqlite3.connect(p)
    c.executescript("CREATE TABLE message (ROWID INTEGER PRIMARY KEY, text TEXT,"
                    " attributedBody BLOB);")
    c.commit()
    c.close()


class Fake:
    name = "fake@local"
    base_url = "http://127.0.0.1:8743"
    model = "m"

    def __init__(self, outputs):
        self.outputs = list(outputs)

    def generate(self, system, user, max_tokens=400):
        out = self.outputs.pop(0)
        if isinstance(out, Exception):
            raise out
        return out


def serve_with(backend_obj):
    @contextlib.contextmanager
    def serve(**kw):
        yield backend_obj
    return serve


def args(tmp_path, *extra):
    return ["--mem-db", str(tmp_path / "mem.db"), "--chat-db", str(tmp_path / "chat.db"),
            "--store", str(tmp_path / "so.db"), "--manifest-dir", str(tmp_path / "logs"),
            "--reports-dir", str(tmp_path / "reports"), "--lock", str(tmp_path / "lock"),
            "--blind-ab-root", str(tmp_path / "ab"), "--jobs", "audit", *extra]


def setup(tmp_path):
    mk_mem(str(tmp_path / "mem.db"))
    mk_chat(str(tmp_path / "chat.db"))


def no_att(*a, **k):
    return {"timelines": {}, "labeled": {}}


def test_happy_path_writes_counts_only_manifest(tmp_path):
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake(["supported\nok"])), attribute=no_att)
    assert rc == 0
    man_path = tmp_path / "logs" / "second-opinion-20260929.json"
    text = man_path.read_text()
    man = json.loads(text)
    assert man["audit"]["audited"] == 1 and man["backend"] == "fake@local"
    assert "SECRET-NOTE" not in text and "+1a" not in text and "priya" not in text
    assert sqlite3.connect(tmp_path / "so.db").execute("SELECT COUNT(*) FROM runs").fetchone()[0] == 1


def test_unreadable_db_refuses_and_writes_nothing(tmp_path):
    mk_chat(str(tmp_path / "chat.db"))                     # no memory.db
    rc = run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 2
    assert not (tmp_path / "logs").exists() and not (tmp_path / "so.db").exists()


def test_server_that_never_comes_up_refuses(tmp_path):
    setup(tmp_path)

    @contextlib.contextmanager
    def broken(**kw):
        raise be.BackendError("not healthy")
        yield

    assert run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING, serve=broken,
                            attribute=no_att) == 2
    assert not (tmp_path / "logs").exists()


def test_window_closed_writes_nothing(tmp_path):
    setup(tmp_path)
    late = dt.datetime(2026, 9, 29, 9, 30).astimezone()
    rc = run_nightly.main(args(tmp_path, "--deadline", "09:00"), now_local=late,
                          serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 0 and not (tmp_path / "logs").exists() and not (tmp_path / "so.db").exists()


def test_second_concurrent_run_does_nothing(tmp_path):
    setup(tmp_path)
    with open(tmp_path / "lock", "w") as held:
        fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
        rc = run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                              serve=serve_with(Fake([])), attribute=no_att)
    assert rc == 0 and not (tmp_path / "logs").exists()


def test_every_item_failing_exits_3_and_keeps_the_manifest(tmp_path):
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake([TimeoutError()])), attribute=no_att)
    assert rc == 3
    man = json.loads((tmp_path / "logs" / "second-opinion-20260929.json").read_text())
    assert man["audit"]["errors"] == 1 and man["exit_reason"] == "every item failed"


def test_dry_run_writes_no_rows_and_suffixes_manifest(tmp_path):
    setup(tmp_path)
    rc = run_nightly.main(args(tmp_path, "--dry-run"), now_local=LOCAL_MORNING,
                          serve=serve_with(Fake(["supported"])), attribute=no_att)
    assert rc == 0 and not (tmp_path / "so.db").exists()
    assert (tmp_path / "logs" / "second-opinion-20260929-dryrun.json").exists()


def test_auto_jobs_add_judge_and_report_on_sunday():
    assert run_nightly.resolve_jobs("auto", dt.date(2026, 9, 29)) == ["audit", "gold"]
    assert run_nightly.resolve_jobs("auto", dt.date(2026, 10, 4)) == ["audit", "gold", "judge",
                                                                       "report"]
    assert run_nightly.resolve_jobs("audit,report", dt.date(2026, 9, 29)) == ["audit", "report"]
```

- [ ] **Step 2: Run the tests; they should fail**

Run: `python3 -m pytest -q tests/test_second_opinion_run_nightly.py`
Expected: FAIL with `ImportError`.

- [ ] **Step 3: Write the implementation**

```python
# scripts/second_opinion/run_nightly.py
"""Nightly runner for the second-opinion lane (spec §5, §6).

cd scripts && python3 -m second_opinion.run_nightly --deadline 09:00
Exit: 0 done / window closed / another run holds the lock; 2 refused (nothing
written); 3 every attempted item failed (manifest still written)."""
import argparse
import contextlib
import datetime as dt
import fcntl
import json
import os
import sqlite3
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from insight_stream import resolve_deadline  # noqa: E402

from . import audit, backend as be, gold, judge, store  # noqa: E402

HOME = os.path.expanduser("~")
JOBS = ("audit", "gold", "judge", "report")


def resolve_jobs(spec, today):
    if spec == "auto":
        return ["audit", "gold"] + (["judge", "report"] if today.weekday() == 6 else [])
    jobs = [j.strip() for j in spec.split(",") if j.strip()]
    bad = [j for j in jobs if j not in JOBS]
    if bad:
        raise SystemExit(f"unknown jobs: {bad}")
    return jobs


def _ro(path):
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    con.execute("SELECT name FROM sqlite_master LIMIT 1").fetchall()
    return con


def _atomic_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1, sort_keys=True)
    os.replace(tmp, path)


def _parse(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--jobs", default="auto")
    ap.add_argument("--judge-run-dir")
    ap.add_argument("--blind-ab-root", default=os.path.join(HOME, "blind_ab_run"))
    ap.add_argument("--backend", choices=["gemma", "vertex"], default="gemma")
    ap.add_argument("--deadline", help="HH:MM local; stop between items after this")
    ap.add_argument("--audit-limit", type=int, default=25)
    ap.add_argument("--gold-limit", type=int, default=10)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    ap.add_argument("--manifest-dir", default=os.path.join(HOME, ".human/logs"))
    ap.add_argument("--reports-dir", default=os.path.join(HOME, ".human/logs/second-opinion-reports"))
    ap.add_argument("--mem-db", default=os.path.join(HOME, ".human/memory.db"))
    ap.add_argument("--chat-db", default=os.path.join(HOME, "Library/Messages/chat.db"))
    ap.add_argument("--lock", default=os.path.join(HOME, ".human/second_opinion.lock"))
    ap.add_argument("--dry-run", action="store_true")
    return ap.parse_args(argv)


def _default_attribute(chat_path, mem_path, since):
    import eval_conversation_quality as cq
    return cq.attribute(chat_path, mem_path, since)


def main(argv=None, *, now_local=None, serve=be.serve_gemma, attribute=None):
    a = _parse(argv)
    now_local = now_local or dt.datetime.now().astimezone()
    attribute = attribute or _default_attribute
    jobs = resolve_jobs(a.jobs, now_local.date())

    deadline = None
    if a.deadline:
        deadline = resolve_deadline(a.deadline, now_local)
        if deadline is None:
            print("window closed; nothing to do", file=sys.stderr)
            return 0

    os.makedirs(os.path.dirname(a.lock) or ".", exist_ok=True)
    lock = open(a.lock, "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("another second-opinion run holds the lock; nothing to do", file=sys.stderr)
        return 0

    try:
        mem = _ro(a.mem_db)
        chat = _ro(a.chat_db)
    except sqlite3.Error as e:
        print(f"refusing: memory.db or chat.db unreadable ({e})", file=sys.stderr)
        return 2

    if a.backend == "vertex":
        ctx = contextlib.nullcontext(be.VertexBackend(allow=True))
    else:
        ctx = serve(log_path=os.path.join(a.manifest_dir, "second-opinion-gemma.log")
                    if not a.dry_run else None)

    started = time.monotonic()
    man = {"jobs": jobs, "dry_run": a.dry_run, "exit_reason": "ok"}
    try:
        with ctx as backend:
            con = store.open_store(":memory:" if a.dry_run else a.store)
            run_id = store.start_run(con, backend.name, ",".join(jobs), store.now_ms())
            man["backend"] = backend.name
            attempted = errors = 0
            if "audit" in jobs:
                man["audit"] = audit.audit_pass(con, backend, mem, chat, a.audit_limit, deadline)
                attempted += man["audit"]["attempted"]
                errors += man["audit"]["errors"]
            if "gold" in jobs:
                since = now_local - dt.timedelta(days=7)
                try:
                    att = attribute(a.chat_db, a.mem_db, since)
                except Exception:
                    att = None  # critiques still run; reference replies need attribution
                    man["gold_attribution_error"] = 1
                runs = [d for d in [judge.latest_run_dir(a.blind_ab_root)] if d]
                man["gold"] = gold.gold_pass(con, backend, runs, att, mem, a.gold_limit, deadline,
                                             wide_live=os.environ.get("HU_INSIGHT_WIDE") == "live")
                attempted += man["gold"]["attempted"]
                errors += man["gold"]["errors"]
            if "judge" in jobs:
                run_dir = a.judge_run_dir or judge.latest_run_dir(a.blind_ab_root)
                if run_dir is None:
                    man["judge"] = {"skipped": "no rating sheet"}
                else:
                    stamp = now_local.strftime("%Y%m%d")
                    out = os.path.join(a.reports_dir, f"judge-{stamp}")
                    try:
                        man["judge"] = judge.judge_pass(backend, run_dir, out)
                    except Exception as e:
                        man["judge"] = {"error": type(e).__name__}
                    if not a.dry_run and "calibration" in man["judge"]:
                        _atomic_json(os.path.join(a.reports_dir, f"judge-{stamp}.json"),
                                     man["judge"]["calibration"])
            if "report" in jobs and not a.dry_run:
                week_ago = store.now_ms() - 7 * 86400 * 1000
                stamp = now_local.strftime("%Y%m%d")
                _atomic_json(os.path.join(a.reports_dir, f"audit-{stamp}.json"),
                             audit.audit_report(con, week_ago))
                _atomic_json(os.path.join(a.reports_dir, f"gold-{stamp}.json"),
                             gold.gold_report(con, week_ago))
            rc = 0
            if attempted > 0 and errors == attempted:
                rc = 3
                man["exit_reason"] = "every item failed"
            man["elapsed_s"] = round(time.monotonic() - started, 1)
            store.finish_run(con, run_id, rc, json.dumps(man), store.now_ms())
    except be.BackendError as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2
    suffix = "-dryrun" if a.dry_run else ""
    _atomic_json(os.path.join(a.manifest_dir,
                              f"second-opinion-{now_local.strftime('%Y%m%d')}{suffix}.json"), man)
    return rc


if __name__ == "__main__":
    sys.exit(main())
```

Note for the implementer: the `with ctx as backend:` block raises `BackendError` while the server is starting, before any store is opened or manifest written. So a server that never comes up writes nothing (test `test_server_that_never_comes_up_refuses`). In dry-run the store is `:memory:`, so no rows persist.

- [ ] **Step 4: Run the tests; they should pass, then run the whole lane**

Run: `python3 -m pytest -q tests/test_second_opinion_run_nightly.py && python3 -m pytest -q tests/test_second_opinion_*.py`
Expected: `8 passed`, then all second-opinion tests pass (46).

- [ ] **Step 5: Commit**

```bash
git add scripts/second_opinion/run_nightly.py tests/test_second_opinion_run_nightly.py
git commit -m "feat(second-opinion): nightly runner — lock, deadline, refusals, counts-only manifest"
```

### Task 9: CI coverage and operator docs

**Files:**
- Modify: `.github/workflows/ci.yml` (the "Insight-stream + curator pins" step added by #530)
- Create: `scripts/second_opinion/README.md`

- [ ] **Step 1: Extend the CI step** so the second-opinion tests run with the curator tests:

```yaml
      - name: Insight-stream + curator + second-opinion pins (hermetic, no chat.db, no model)
        run: python3 -m pytest -q tests/test_insight_stream_*.py tests/test_curator_*.py tests/test_second_opinion_*.py
```

(Replace the existing step's `name` and `run` lines; keep its comment and update it to mention the second-opinion lane.)

- [ ] **Step 2: Write `scripts/second_opinion/README.md`**

```markdown
# Second-opinion lane

A different model family (Gemma 4 31B, local on :8743) checks h-uman overnight.
Spec: `docs/superpowers/specs/2026-09-29-second-opinion-lane-design.md`.

Run: `cd scripts && python3 -m second_opinion.run_nightly --deadline 09:00`
(`--jobs auto` = audit + gold daily, + judge + report on Sundays; `--dry-run` writes no rows;
`--backend vertex` sends message excerpts to Vertex and is opt-in only).

One-time calibration after ~2 weeks:
`python3 -m second_opinion.audit_sheet` → Seth fills `supported` (y/n) →
`python3 -m second_opinion.audit_score ~/.human/second_opinion/audit_check.csv --key ~/.human/second_opinion/audit_check.key.json`.

Rate reference replies: `python3 -m second_opinion.gold_rate --write rate.csv`, fill `good`
(y/n), then `--import rate.csv`. Export only rated-good rows with
`python3 -m second_opinion.gold_export out.csv`.

Store: `~/.human/second_opinion.db` (0600). Manifests: `~/.human/logs/second-opinion-YYYYMMDD.json`
(counts only). Reports: `~/.human/logs/second-opinion-reports/`.
```

- [ ] **Step 3: Verify locally**

Run: `python3 -m pytest -q tests/test_insight_stream_*.py tests/test_curator_*.py tests/test_second_opinion_*.py && python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"`
Expected: all pass; the YAML parses.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/ci.yml scripts/second_opinion/README.md
git commit -m "ci+docs(second-opinion): run the lane's tests in CI; operator README"
```

---

## After merge (operator steps, not in the repo)

1. Download the model: `~/Documents/gemma-realtime-1/.venv312/bin/python -c "from huggingface_hub import snapshot_download; snapshot_download('mlx-community/gemma-4-31b-it-4bit')"` (about 18 GB).
2. Smoke test (spec §7):
   ```
   cd scripts && python3 -m second_opinion.run_nightly --jobs audit --audit-limit 3
   ```
   Check for 3 rows in `audits`, then check the manifest, then confirm nothing is listening on `:8743` afterwards.
3. Add launchd `ai.human.second-opinion`, running daily at 07:40:
   ```
   cd /Users/sethford/Projects/h-uman/scripts && /opt/homebrew/bin/python3 -m second_opinion.run_nightly --deadline 09:00
   ```
   Back up any existing plist first.
4. After about 2 weeks: run `python3 -m second_opinion.audit_sheet` and ask Seth for the one-time 30-row check.
