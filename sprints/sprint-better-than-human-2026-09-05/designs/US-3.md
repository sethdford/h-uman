# Design for US-3: Seth's own initiation-response baseline from chat.db

> STATUS: DESIGNED — verified against the current codebase (grep/live-probe citations
> inline). Supersedes the skeleton; the skeleton's core assumption (`FIR_WINDOW_HOURS`
> importable from `eval_when_to_speak.py`) was **UNVERIFIED and turned out FALSE** — see
> Finding 1. All other skeleton claims held up.

## Scope note (read first)

The dispatch brief asked for both a "false-initiation rate" and a "missed-initiation
rate." **US-3's actual acceptance criteria (stories.md AC-3.1–AC-3.6) define only one
metric**: the fraction of Seth's own conversation-opening sends that go unanswered — the
direct analog of `eval_when_to_speak.py`'s FIR, not MIR. There is no Seth-side MIR analog
in this story (MIR measures *missed* replies to inbound messages, which is a different
question — "did Seth reply to people," not "did people reply to Seth's opener" — and
would need its own AC/estimate). This design implements exactly AC-3.1–AC-3.6 (the
unanswered/false-initiation rate) and calls the second concept out explicitly here rather
than silently expanding scope. If a Seth-side MIR analog is wanted, it is a separate,
similarly-sized follow-up story, not an add-on to this one.

## Finding 1 (blocks the skeleton's approach): `FIR_WINDOW_HOURS` does not exist yet

**VERIFIED** by grep:
```
$ grep -n "FIR_WINDOW_HOURS" scripts/eval_when_to_speak.py
17:    engagement (the contact did not reply within FIR_WINDOW_HOURS)?
$ grep -n "^[A-Z_]* = " scripts/eval_when_to_speak.py
53:APPLE_EPOCH = 978307200
```
`FIR_WINDOW_HOURS` is only prose in the module docstring. The actual value lives as an
argparse default inside `main()`:
```python
ap.add_argument("--fir-window-hours", type=float, default=24.0, ...)   # line 301-302
```
`from scripts.eval_when_to_speak import FIR_WINDOW_HOURS` (the skeleton's plan) would
raise `ImportError`. AC-3.1 requires the two rates to "share the same window definition,"
which is only possible if the value is a real importable module-level name.

**Fix (small, additive, low-conflict — part of this story's file list below):** add one
module-level constant to `scripts/eval_when_to_speak.py` and point the existing argparse
default at it:
```python
FIR_WINDOW_HOURS = 24.0  # contact-replied-within window for FIR; also the comparison
                          # window for scripts/eval_seth_initiation_baseline.py's rate.
...
ap.add_argument("--fir-window-hours", type=float, default=FIR_WINDOW_HOURS, ...)
```
No other file references `--fir-window-hours` or `fir_window_hours` — **VERIFIED**:
```
$ grep -rn "fir-window-hours\|fir_window_hours" --include=*.py --include=*.sh --include=*.plist --include=*.md .
scripts/eval_when_to_speak.py:301,353,371   (the file itself, all consistent with the change)
docs/research/2026-09-02-august-2026-sota-gap-analysis.md:121   (cites FIR=0.670 as prose, not code)
```
So the argparse behavior and CLI surface are unchanged; only a name now exists for other
modules to import. This is the one piece of this story's diff that lands outside the new
files — see **Conflicts with other stories** below for the US-4 interaction.

## Reuse precedent (VERIFIED, not invented)

Two existing scripts already import directly from `eval_when_to_speak.py` with the same
two-line `sys.path.insert` idiom — this is an established pattern in this codebase, not a
new one:
```
$ grep -rn "from eval_when_to_speak import" scripts/*.py
scripts/eval_reply_delay_model.py:90:from eval_when_to_speak import open_ro
scripts/fit_reply_delay_model.py:43:from eval_when_to_speak import APPLE_EPOCH, open_ro, apple_ns_to_unix, load_dm_messages
```
`scripts/fit_reply_delay_model.py:38-43` is the exact template to copy:
```python
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
from eval_when_to_speak import APPLE_EPOCH, open_ro, apple_ns_to_unix, load_dm_messages, FIR_WINDOW_HOURS  # noqa: E402
from score import wilson  # noqa: E402
```
This gets the new script `open_ro()` (the `mode=ro` URI helper — **VERIFIED**
`scripts/eval_when_to_speak.py:71-74`), `load_dm_messages()` (the DM-chat-only,
`is_system_message=0 AND item_type=0` query — **VERIFIED** lines 80-124), and
`apple_ns_to_unix()`/`APPLE_EPOCH` (**VERIFIED** lines 53-59), **without re-deriving the
chat.db schema query a fourth time** in this codebase (it already exists in
`eval_when_to_speak.py`, `eval_reply_delay_model.py`, `fit_reply_delay_model.py`). `wilson()`
is **VERIFIED** at `scripts/blind_ab/score.py:28-36`, signature `wilson(k, n, z=1.96) ->
(p, lo, hi)`, already imported cross-directory by `eval_persona_evolution.py:98-99` using
the identical `blind_ab` sys.path insert (though for a different symbol,
`decode_attributed_body`) — same idiom, different name.

`load_dm_messages()` returns `[(chat_id, contact, ts_unix, is_from_me), ...]` for every
message (both directions) in 1:1 chats since a given cutoff — exactly the shape this
story needs; no new chat.db query has to be written at all, only new logic over the
returned rows.

## Metric definition

### "Initiation"

An outbound (`is_from_me=1`) message in a DM chat (`load_dm_messages`'s definition: a
`chat_id` joined to exactly one `handle_id` via `chat_handle_join` — **VERIFIED**
`eval_when_to_speak.py:88-98`) counts as an **initiation** iff, within that same
`chat_id`'s message stream sorted by `ts`, either:

- it is the first message ever recorded for that chat (no earlier row at all), or
- the immediately preceding message in that chat (from either party) is **≥
  `INITIATION_GAP_HOURS`** before it.

Grouping is by raw `chat_id`, not by contact-merged-across-chats. This is a deliberate
departure from `eval_when_to_speak.py`'s own `contact_to_chat()` helper (lines 164-170),
which collapses a contact to a single arbitrary `chat_id` via `dict.setdefault` — a known
simplification in that file (relevant when joining to a *separate* decision log keyed by
contact, which this story does not need to do). Operating directly on `chat_id` avoids
inheriting that simplification and is strictly more correct for a chat.db-only
measurement: if a contact has two chat threads (e.g. an SMS/iMessage split), each is
evaluated on its own silence, not merged.

**`INITIATION_GAP_HOURS = 6.0`** — justification: this reuses the *value* (not the name;
see below) of `eval_when_to_speak.py`'s own `--seth-already-engaged-hours` default
(**VERIFIED** line 307-309: "FIR exclusion window: Seth sent something in this window
before the daemon's send"). That flag encodes the same underlying judgment this story
needs — "a gap this short means still the same conversation, not a fresh opener" — just
applied from the daemon's side. Using the same number keeps the two scripts'  intuitions
aligned without inventing an unrelated threshold. It is **not** imported as a shared
constant: `--seth-already-engaged-hours` is a CLI default inside `eval_when_to_speak.py`'s
`main()`, not a module-level name, and promoting it to one is out of scope for this story
(the only cross-file coupling AC-3.1 asks for is the FIR window — adding a second one
would widen the diff to `eval_when_to_speak.py` and the US-4 conflict surface for no ask
in the ACs). `INITIATION_GAP_HOURS` is defined locally in the new script with a comment
citing the precedent value and its file:line.

### "Unanswered" / false-initiation

Of the initiations, one is **unanswered** iff no inbound (`is_from_me=0`) message arrives
in the *same* `chat_id` within **`FIR_WINDOW_HOURS`** (imported, per Finding 1 — this is
the entire point of AC-3.1: comparable to the daemon's own FIR on the same window).

`rate = unanswered / n`, with a 95% Wilson CI via the imported `wilson()`.

### Sensitivity (measured live, 2026-09-05 — see Finding 2 for why these numbers will drift)

Real counts from the machine's current `chat.db`, DM chats only, no message content
touched — grouped by `chat_id` (internal integer, never printed) with the rule above:

| INITIATION_GAP_HOURS (X) | FIR_WINDOW_HOURS (Y) | n (initiations) | unanswered | rate |
|---|---|---|---|---|
| 3 | 24 | 61 | 18 | 0.295 |
| **6** | **24 (recommended)** | **50** | **16** | **0.320** |
| 12 | 24 | 41 | 16 | 0.390 |
| 24 | 24 | 31 | 14 | 0.452 |

(Y=12 and Y=48 were also measured; the rate is not sensitive to Y beyond ~24h — 0-2 rows
move between 12/24/48 — because most replies that come at all come within a few hours.
The rate IS sensitive to X, which is expected: a larger silence-gap threshold selects
fewer, "more genuinely cold" openers, and those go unanswered more often. This is
consistent with the FIR intuition, not a red flag.) At the recommended X=6/Y=24 pair,
**n=50 clears `--min-n 30` today**; at X=24 the margin over the refusal floor is thin
(n=31) — a few more days of retention roll (Finding 2) could push it under 30. This
table is the sensitivity justification the story asks for; it is not itself the
committed artifact (the implementer runs the real script for that).

## Finding 2: the "30-day retention floor" is a moving target, not a fixed date

The skeleton and `docs/plans/2026-09-02-persona-evolution/spec.md` §3 cite **2026-08-03**
as the retention floor (**VERIFIED** in spec.md:77-78, dated 2026-09-02). Per this
session's memory (`MEMORY.md`), the floor had already rolled to 2026-08-05 by
2026-09-03. **Live-probed just now** (read-only, counts only):
```
min(is_from_me=1 date) = 2026-08-06T14:42:13 UTC
max(is_from_me=1 date) = 2026-09-05T15:41:34 UTC   (today; live)
total is_from_me=1 (all chats)      = 1013
total is_from_me=1 (DM chats only)  = 812
DM chats total / 1:1 subset          = 2541 / 2251
```
The floor advances roughly one day per day (a rolling "Keep Messages" retention window),
so **the available n a future run sees will not match the table above** — it is
illustrative of the operationalization, not a promise about what AC-3.4's real run will
report. The design accounts for this by having the script report its own actual observed
date range every run (AC-3.4), never a hardcoded one, and by not asserting today's n=50
anywhere as if it were durable.

## Refusal conditions

Exit non-zero, write nothing, in this order:

1. `chat.db` not found at `--chat-db` path → `REFUSE: chat.db not found at <path>` (mirrors
   `eval_when_to_speak.py:320-322`).
2. Zero DM messages returned by `load_dm_messages()` → `REFUSE: no DM messages found`.
3. **n (initiations) < `--min-n` (default 30)** → `REFUSE: insufficient n (n=<n>,
   min_n=30) — not writing a result.` This is AC-3.3's required outcome and, per Finding 2,
   a real possible outcome on a future run, not just a test fixture — treat it as
   acceptable and expected, not a bug.

`--min-n` default of 30 matches `eval_when_to_speak.py`'s own `--min-n` default
(**VERIFIED** line 310) and `fit_reply_delay_model.py`'s (per its docstring, line 30) —
not a new magic number.

## Files touched

| File | Change | LOC (est.) |
|---|---|---|
| `scripts/eval_when_to_speak.py` | Add `FIR_WINDOW_HOURS = 24.0` module constant; point the existing `--fir-window-hours` argparse default at it. No behavior change. | +2 / -1 |
| `scripts/eval_seth_initiation_baseline.py` | New script (see below) | ~180 |
| `scripts/test_eval_seth_initiation_baseline.py` | New hermetic tests | ~200 |

### `scripts/eval_seth_initiation_baseline.py` — functions

```python
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
from eval_when_to_speak import open_ro, load_dm_messages, FIR_WINDOW_HOURS  # noqa: E402
from score import wilson  # noqa: E402

INITIATION_GAP_HOURS = 6.0  # see design doc: mirrors eval_when_to_speak.py's
                             # --seth-already-engaged-hours default (line 307-309)

def find_initiations(messages, gap_hours=INITIATION_GAP_HOURS):
    """messages: [(chat_id, contact, ts, is_from_me), ...] from load_dm_messages().
    Returns [{"chat_id": ..., "ts": ...}, ...] for every outbound message that is
    either the first row in its chat_id or preceded by a gap >= gap_hours. Grouped
    and sorted per chat_id internally; contact/chat_id never leave this module's
    output (see compute_baseline)."""

def label_unanswered(initiations, messages, window_hours=FIR_WINDOW_HOURS):
    """Adds "unanswered": bool to each initiation dict — True iff no is_from_me=0
    row in the same chat_id arrives within window_hours after ts."""

def compute_baseline(labeled):
    """Returns {"n": int, "unanswered": int, "rate": float|None,
    "wilson_ci": [lo, hi]} using wilson(unanswered, n) — None/[0,0] rate/CI when n==0
    (caller refuses before this is reached in practice, but the function itself
    stays total)."""

def main():
    """argparse (--days default 90 [retention naturally bounds it further, see
    Finding 2], --gap-hours default INITIATION_GAP_HOURS, --fir-window-hours default
    FIR_WINDOW_HOURS, --min-n default 30, --chat-db, --out-dir), refusal checks in the
    order above, JSON written to
    ~/.human/logs/seth-initiation-baseline-<date>.json mirroring
    eval_when_to_speak.py's own out-path convention (line 386-390), stdout summary.
    Output JSON fields: generated_at, days, gap_hours, fir_window_hours, date_range
    {first, last} (ISO date strings, no time-of-day needed beyond date precision),
    n, unanswered, rate, wilson_ci. No chat_id, no contact hash, no text — the
    aggregation functions above operate on chat_id/contact internally but nothing
    downstream of compute_baseline() ever receives them."""
```

## Privacy (AC-3.5)

- Only `open_ro()` (imported, enforces `mode=ro&immutable=1` — **VERIFIED**
  `eval_when_to_speak.py:71-74`) ever touches `chat.db`.
- `load_dm_messages()`'s `SELECT` (imported, unmodified) reads `date`, `is_from_me`, and a
  join-only `handle.id` for chat grouping — **no `text` or `attributedBody` column is
  selected anywhere in this story's code path** (contrast `eval_persona_evolution.py`,
  which does need message text for style axes and therefore has a `decode_attributed_body`
  step this script has no reason to import).
  cite: `eval_when_to_speak.py:104-113`'s query selects `cmj.chat_id, m.date, m.is_from_me`
  only.
- `contact` (the handle id) and `chat_id` are used only as **in-memory grouping keys**
  inside `find_initiations`/`label_unanswered`; `compute_baseline`'s return value and
  everything written to disk or printed contains only integers/rates/dates.
- No `INSERT`/`UPDATE`/`DELETE` anywhere in the new module — enforced by a static test
  (below), following the `inspect.getsource` pattern already used at
  `scripts/test_training_loop_gates.py:115`.

## Hermetic tests — `scripts/test_eval_seth_initiation_baseline.py`

Build a synthetic sqlite file at `tmp_path` with the minimal real schema
(`message(ROWID, date, is_from_me, is_system_message, item_type)`,
`chat_message_join(chat_id, message_id)`, `chat_handle_join(chat_id, handle_id)`,
`handle(ROWID, id)`) — same shape `load_dm_messages()` queries (imported unmodified, so
these tests exercise the real query, not a stub).

1. `test_refuses_below_min_n` — 5 initiations total (< 30) → `main()`/`run()` returns
   non-zero, no output file written, stderr contains `REFUSE`.
2. `test_known_rate_exact` — construct N=40 initiations with exactly K unanswered by
   direct construction of message timestamps; assert `compute_baseline()["rate"] ==
   K/N` exactly and `wilson_ci` matches `wilson(K, N)` bit-for-bit (call the imported
   `wilson()` in the test to compute the expected tuple — never hardcode a decimal).
3. `test_group_chats_excluded` — a chat with 2 distinct `handle_id`s joined must
   contribute zero initiations (mirrors `load_dm_messages`'s own `HAVING COUNT(DISTINCT
   ...) = 1`, so this also indirectly re-verifies the imported helper still filters
   groups after any future edit to it).
4. `test_first_message_in_chat_is_an_initiation` — a chat whose only message is a single
   outbound send counts as 1 initiation (no prior message to measure a gap against).
5. `test_gap_boundary_exact_hours` — a preceding message at exactly
   `gap_hours * 3600` seconds before is an initiation (`>=`, not `>`); one second short is
   not.
6. `test_window_parity_with_eval_when_to_speak` — `import eval_when_to_speak as ews;
   import eval_seth_initiation_baseline as esib; assert esib.FIR_WINDOW_HOURS is
   ews.FIR_WINDOW_HOURS` (identity, not just equality — proves it's the same imported
   name, not a coincidentally-equal copy). This is AC-3.6's third required case, verbatim.
7. `test_no_write_statements_in_module` — `src = inspect.getsource(esib); assert
   "INSERT" not in src.upper() and "UPDATE" not in src.upper() and "DELETE FROM" not in
   src.upper()` (AC-3.2's static half).
8. `test_open_ro_uri_used` — monkeypatch `sqlite3.connect` (or assert on
   `eval_when_to_speak.open_ro`'s already-tested URI construction, since it's imported,
   not reimplemented) to confirm the connection string contains `mode=ro` and
   `immutable=1`.
9. `test_output_has_no_identifying_fields` — run end-to-end against the synthetic DB,
   `json.load` the written file, assert its top-level keys are exactly the documented
   allowlist (`generated_at, days, gap_hours, fir_window_hours, date_range, n,
   unanswered, rate, wilson_ci`) — an accidental `chat_id`/`contact` leak into the output
   dict fails this test even if nobody thought to grep for it.
10. `test_date_range_reflects_synthetic_data` — asserts `date_range.first`/`.last` match
    the constructed timestamps' dates, not the `--days` argument (AC-3.4).

All ten tests run against an in-memory or `tmp_path` sqlite file — **zero** access to the
real `~/Library/Messages/chat.db`, so the suite is fully hermetic and safe in CI.

## Conflicts with other stories

**US-4 also lists `scripts/eval_when_to_speak.py` as touched** ("read; only touched if the
`proactive_decisions` source-detection branch needs a fix" — stories.md line 148, and its
own skeleton at `designs/US-4.md:14` says the same). This story's one required edit to
that file (the `FIR_WINDOW_HOURS` constant, Finding 1) is a 2-line, additive,
behavior-preserving change nowhere near the `load_decisions()` source-detection branch
(lines 176-227) US-4 would touch. Recommended sequencing: whichever of US-3/US-4 lands
first should land this constant (it's needed either way — US-4's AC-4.4 explicitly
compares FIR against this story's baseline, so the shared name has to exist regardless of
order); the other rebases past a trivial, non-overlapping diff. No file-level lock is
needed beyond normal "smallest diff, land first" discipline — the two changes touch
disjoint line ranges of the same file.

No other story touches `scripts/eval_when_to_speak.py`, `scripts/blind_ab/score.py`, or
any new file this story creates. US-3 has no technical dependency on US-1/US-2 and none
of them touch chat.db.

## Estimate

Confirms the backlog's **S** estimate:
- `scripts/eval_when_to_speak.py`: +2/-1 lines.
- `scripts/eval_seth_initiation_baseline.py`: ~180 LOC (three pure functions + argparse
  main, all built on three already-tested imports — `open_ro`, `load_dm_messages`,
  `wilson`).
- `scripts/test_eval_seth_initiation_baseline.py`: ~200 LOC, 10 hermetic tests, one
  synthetic-schema fixture reused across most of them.
- No C code, no daemon/service-loop interaction, no model load
  (`scripts/check-no-resident-model.sh` is inapplicable — this story never touches a
  provider or training process).
- Total: roughly half a working session (~1-2 hours) for an implementer already oriented
  in `eval_when_to_speak.py`'s conventions; the live-probe numbers above suggest no
  surprises are likely in the real run.

RESULT_tech-lead=READY — design replaces the skeleton with a verified operationalization
(initiation = outbound after ≥6h silence in the same chat_id; false-initiation = no inbound
reply within the imported FIR_WINDOW_HOURS=24h, a constant this design adds to
eval_when_to_speak.py since it did not previously exist), grounded in a live read-only
chat.db probe (n=50, rate=0.32 today, expected to drift per the rolling retention floor),
with the AC-3.1 import bug in the skeleton found and fixed before any code is written.
