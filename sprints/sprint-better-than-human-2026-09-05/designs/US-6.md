# Design for US-6: Preference-based human blind A/B (win rate, not detection)

Status: DESIGNED
Sprint: sprint-better-than-human-2026-09-05
Priority: P2 · Estimate: M

## Context (verified)

The existing pipeline (`scripts/blind_ab/`) asks one question — *"which is the real
Seth?"* — and scores **detection rate** (0.50 = indistinguishable, the goal is chance).
That machinery is:

- `export_seth_triples.py` → `{context, seth_reply}` from `chat.db`, contact **handles**
  aliased to `contact_N` (`export_seth_triples.py:12-14`, alias built at
  `export_seth_triples.py:156-158`). No content-level redaction exists — confirmed by
  `grep -rn "redact" scripts/blind_ab/*.py` returning nothing outside that docstring.
- `gen_huuman_replies.py` → adds `huuman_reply`, producing `triples.json`.
- `make_rating_sheet.py` (`make_rating_sheet.py:19-36` `build()`) → shuffles A/B per
  row, writes `rating_sheet.csv` + a **private** `answer_key.json` mapping
  `id -> "A"|"B"` = **which side is Seth**.
- `score.py` → `score_rows()` (`score.py:85-108`) counts `choice == key[id]` as
  "detected"; `wilson()` (`score.py:28-36`) is the CI; `--rater {human,synthetic}`
  (`score.py:274-281`) gates whether a run may touch the promotion-authoritative
  `~/.human/blind_ab_gate.json`; `detect_rater_kind()` (`score.py:245-257`) vetoes a
  `--rater human` claim on any sheet stamped by `synthetic_judge.py`; the `agg["n"] ==
  0` refusal (`score.py:312-315`) is the precedent this design must reuse, not
  reimplement, for the 07-25 vacuous-PASS class of bug.
- Real human ratings live outside the repo at `~/.human/blind_ab_human/` (confirmed by
  directory listing only, per instructions: contains `rating_sheet.csv`,
  `answer_key.json`, `drip_state.json`, and dated `rated-*`/`archive-*`
  subdirectories — no sheet content read). `rating_drip.py` (`rating_drip.py:13-16,
  39-44`) is the current human-outer-loop mechanism: it feeds ONE detection question at
  a time to Seth's own self-chat and auto-runs `score.py` when the sheet fills.
- `docs/evaluation/blind_ab_gate.json` (`blind_ab_gate.py:14`) is the repo-committed
  gate; `~/.human/blind_ab_gate.json` is the live one the C promotion code reads
  (`score.py:369`). Both are written through `write_human_half`/`write_synthetic_half`
  (`blind_ab_gate.py:196-222`), which stamp `tool` — `human_is_attributable()`
  (`blind_ab_gate.py:45-66`) treats an unstamped "human" PASS as ABSENT.
- **No phone/name redaction exists today.** `export_seth_triples.py` aliases the
  *sender handle*, but a phone number or a friend's first name typed **inside**
  `context`/`seth_reply`/`huuman_reply` text passes through unredacted. AC-6.4 is a
  real gap, not a restatement of existing behavior.
- `scripts/mine_all_data.py:65-101` (`resolve_contacts_from_addressbook()`) and
  `scripts/mine_all_data.py:110-119` (`normalize_phone()`) are the only existing
  phone/name-matching code in the repo — reused below as the *live* name source, never
  as a network call and never baked into a hermetic test.
- US-3 (`stories.md:110`) imports `wilson()` read-only from `score.py` for an unrelated
  baseline. No file-write conflict, but it fixes `wilson()`'s signature
  (`wilson(k, n, z=1.96) -> (p, lo, hi)`) as a **shared contract** — this design must not
  change it, matching AC-6.2's "unmodified" requirement.
- `stories.md:322-323` scopes this as **additive**: the existing 0.60 detection
  threshold and gate are untouched.

## Approach

Add a **preference mode** that is structurally the *same machine* — same triple shape,
same shuffle-then-key mechanic, same `wilson()` math, same `--rater human|synthetic`
provenance split — with two things changed: the **question asked** and **what the key
encodes**. This is possible because `score_rows()`'s core loop
(`score.py:99` `correct = 1 if choice == key[rid] else 0`) is already metric-agnostic —
it does not know or care whether the key means "the real Seth side" or "the
model-preferred side". Detection asks raters to *identify* Seth; preference asks raters
to *prefer* a reply. Both reduce to "did the rater's choice match the key" and are
scored by the identical function. This is why AC-6.2 ("reuse `wilson()` unmodified") and
AC-6.3 ("reuse `--rater human` vs `synthetic`") are satisfiable with zero changes to
`score.py`'s scoring math — only a new sibling script for the preference-specific
reporting/refusal/evidence-write behavior, so a promotion-relevant write path is never
shared between two different metrics.

Two behavior changes are needed either way, in `make_rating_sheet.py`: a new
`--mode {detection,preference}` flag (default `detection`, so every existing caller and
test is unaffected) that (a) swaps the instructions text so the two framings can never
be conflated in one sheet (AC-6.1), and (b) applies content redaction to `context`,
`seth_reply`, and `huuman_reply` before they are written to any CSV, in **both** modes
(the gap is real for detection sheets too, but AC-6.4 only requires it be present, and
since the code path is shared there is no reason to gate it to one mode).

## Files touched

| File | Change |
|---|---|
| `scripts/blind_ab/make_rating_sheet.py` | Add `--mode {detection,preference}` (default `detection`); add `redact(text, name_list)` applied to `context`/`seth_reply`/`huuman_reply` before any row is built; when `--mode preference`, `key[id]` stores which side is the **model** reply, and a top-level `"_mode": "preference"` marker is added to `answer_key.json` (JSON dict, additive key, invisible to the existing `{id: "A"|"B"}` consumers that only index by triple id); skip (and count) any triple where `seth_reply.strip() == huuman_reply.strip()` after redaction — a real-vs-real pair defeats a preference measurement — and refuse (exit non-zero, write nothing) if that leaves zero usable rows. |
| `scripts/blind_ab/score_preference.py` (new) | Thin wrapper: `from score import wilson, score_rows, load_sheets, detect_rater_kind`. Computes win rate via `score_rows()` unmodified (the "detect" field is renamed only in the report, not recomputed), applies preference-specific refusal (n<20, non-human rater for evidence writes, `_mode` mismatch, mixed real/real contamination already caught upstream), and writes the evidence JSON. **Never touches `~/.human/blind_ab_gate.json` or `docs/evaluation/blind_ab_gate.json`** — this is a new, separate, non-promotion-gating measurement (`stories.md:322-323`). |
| `scripts/blind_ab/PROTOCOL.md` | New `## Preference measurement (win rate)` section, clearly separated (own heading, own pass/fail language) from the existing detection section so raters and future readers cannot conflate "which is real" with "which do you prefer" in the same sheet. |
| `scripts/blind_ab/test_score.py` | Extend `TestRaterGateSeparation`-style hermetic subprocess tests, but for `score_preference.py`: n<20 refusal, non-human rater refuses evidence write, provenance-stamped sheet refuses `--rater human` (reuse `detect_rater_kind` fixture pattern at `test_score.py:410-424`). |
| `scripts/blind_ab/test_make_rating_sheet.py` (new — not in stories.md's file list, justified below) | `redact()` unit tests (phone-shaped, name-shaped, both modes); `--mode preference` key-shape test; real/real-pair skip-and-count test; refusal-on-all-duplicate test. |
| `sprints/sprint-better-than-human-2026-09-05/evidence/US-6/` (new dir) | `preference-results-<date>.json` — the one committed AC-6.5 artifact: `{n, win_rate, ci_lo, ci_hi, rater, date}`. No raw sheet, no message text. |

Deviation from `stories.md:222-224`'s file list: it names `test_score.py` only; I split
redaction/mode-building tests into a new `test_make_rating_sheet.py` because
`make_rating_sheet.py` currently has **zero** test coverage (confirmed:
`ls scripts/blind_ab/ | grep test_make_rating_sheet` → no match) and AC-6.4's assertion
belongs next to the code it tests, not bolted onto `test_score.py`'s axis-scoring suite.

## Rater flow / privacy

1. Reuse steps 1-2 unchanged (`export_seth_triples.py`, `gen_huuman_replies.py`) —
   already alias contact handles and never leave the machine.
2. `make_rating_sheet.py triples.json --mode preference --out-dir ~/blind_ab_run`
   writes `rating_sheet.csv` (redacted) + `answer_key.json` (redacted-text-derived,
   `_mode: "preference"`) to a **non-repo** path, matching the existing
   `~/.human/blind_ab_human/` pattern confirmed by directory listing.
3. New instructions text (`PROTOCOL.md`'s new section, or a sibling
   `RATER_INSTRUCTIONS_PREFERENCE.txt` modeled on the existing
   `RATER_INSTRUCTIONS.txt:1-16`) asks: *"Which of these two replies would you rather
   receive from Seth — not which one IS Seth, which one do you PREFER?"* Confidence
   1-5 stays, same column.
4. Raters are the same pool PROTOCOL.md already specifies — "5-8 people who genuinely
   know Seth" — never strangers, because preference judgment needs the same
   familiarity instrument detection does. `stories.md:332-334`'s open question (same
   pool + same trial pairs, vs. a fresh batch) is answered here as: **reuse the same
   exported triples, generate a fresh preference-framed sheet from them** (same
   `context`/`seth_reply`/`huuman_reply`, different `--mode`) rather than re-exporting
   from `chat.db` — this avoids a second privacy-sensitive export and answers "same
   time budget" by not asking raters to review new content, only to answer a
   differently-framed question about content they may have already seen. Flagging this
   as the assumed default; the stakeholder open question in `stories.md:332-334` about
   rater *time* budget (a second full pass, even over the same items) remains open and
   is not resolved by this design.
5. `score_preference.py` reads the completed sheet back; the repo never sees raw text,
   only the aggregate evidence JSON (AC-6.5).

**Redaction (`redact()` in `make_rating_sheet.py`, new function):**
- Phone-shaped: regex over digit runs, reusing the normalization idea from
  `scripts/mine_all_data.py:110-119` (`normalize_phone`) — match `\(?\d{3}\)?[-.\s]?\d{3}[-.\s]?\d{4}` and international `+\d{7,15}` forms, replace with `[phone]`.
- Name-shaped: build a name set from `resolve_contacts_from_addressbook()`
  (`scripts/mine_all_data.py:65-101`) **at generation time only** (never inside a
  test — that function does a live macOS AddressBook read), split each contact's
  first+last name into tokens, and replace any exact case-insensitive token match in
  the text with `[name]`. This is deliberately a real-name-list match, not a
  capitalized-word heuristic (which would also strip "Friday", "God", sentence-initial
  words, etc. — a known false-positive trap `.claude/rules/substring-classifier-pitfalls.md`
  warns about generally). Tests inject a **fixture** name list (`["Sarah", "Jake"]`)
  rather than reading the real AddressBook, keeping the test hermetic.
- Applied to `context`, `seth_reply`, `huuman_reply` before either is placed into
  `option_A`/`option_B` — so redaction is symmetric and cannot itself become a tell
  (e.g., only one side redacted would leak which side is real/model).

## The statistic

`win_rate = wins / n` where `wins` = count of rows where the rater's `choice` equals
`key[id]` and `key[id]` now means **"the model's side."** Computed by calling
`score_rows(rows, key)` (`score.py:85-108`) verbatim — no fork of that function — and
reading its `detect`/`ci_lo`/`ci_hi`/`n` fields under new names in the report layer
only:

```python
from score import score_rows, wilson  # unmodified imports, score_preference.py
agg = score_rows(rows, key)            # key[id] == model's side under preference mode
win_rate, ci_lo, ci_hi, n = agg["detect"], agg["ci_lo"], agg["ci_hi"], agg["n"]
```

`wilson(k, n)` (`score.py:28-36`) is the CI source, called internally by
`score_rows()` — this satisfies AC-6.2 exactly ("reuse `wilson()` unmodified") without
score.py needing to know the word "preference" exists.

Bootstrap CI is **not** needed in addition to Wilson: the skeleton's "Wilson/bootstrap"
hedge is resolved in favor of Wilson alone, because the underlying data is a simple
binomial (each item is one independent A/B pick) — exactly what Wilson is for and what
the existing detection path already trusts. Adding a second CI method for the same
binomial quantity would be unjustified complexity with no reviewer asking for it.

## Refusal conditions (score_preference.py)

Mirrors `score.py`'s existing refusal shapes (same file, same author, same
`no-number-without-a-measurement.md` discipline) rather than inventing new ones:

1. **n < 20 pairs** → refuse (exit non-zero), print `RESULT_blind_ab_preference=INVALID (n=<n> < 20)`, write no evidence file. Chosen because AC-6.5 requires "at least one real run (n≥20 pairs)" — 20 is the floor the acceptance criterion itself sets, not an arbitrary import from the 0.60-detection protocol (which has no n floor documented in `PROTOCOL.md`).
2. **Non-`human` rater tag on an evidence-writing invocation** → reuse `detect_rater_kind()` (`score.py:245-257`) unmodified; a sheet stamped `judge_api`/`judge_model` refuses `--rater human` for the same fail-safe reason `score.py:297-303` refuses it today. Unlike detection, there is no "synthetic half" concept for preference (no gate file exists to have two halves) — a non-human sheet may still be scored and printed (useful for synthetic-judge dry runs of the new framing) but the evidence JSON under `sprints/.../evidence/` is written **only** when `--rater human`.
3. **Unparsed/blank `choice`** → already handled: `score_rows()`'s own row loop (`score.py:90-92`) skips any row whose `choice` isn't `"A"`/`"B"`; those rows simply don't count toward `n`. No new code needed, but the n<20 check runs on the *post-filter* `n`, so a sheet with 30 rows and 15 blank answers correctly refuses at n=15.
4. **Real/real contamination ("mixed real/real pair")** — handled **upstream**, at sheet-build time in `make_rating_sheet.py`, not at scoring time: if `seth_reply` and `huuman_reply` are byte-identical after redaction (the shape of the "Dermot parrot incident" — a generation bug that echoes the input back as the model's own reply, per `dermot_parrot_incident.md` in memory), that triple has no real "model side" to prefer and is **excluded from the sheet** with a printed count (`"skipped N/M triples: seth_reply == huuman_reply (no model side to compare)"`). If exclusion leaves zero rows, `make_rating_sheet.py` refuses the whole build (exit non-zero, no CSV/key written) rather than shipping raters a sheet with nothing real to rate — same shape as `gen_classifier_trials.py`'s `--min-ok` refusal (`gen_classifier_trials.py:190-192`).
5. **Never write a promotion-relevant artifact from a synthetic sheet** — enforced structurally, not just by a flag check: `score_preference.py` has no code path that touches `~/.human/blind_ab_gate.json` or `docs/evaluation/blind_ab_gate.json` at all (unlike `score.py`, which writes both unconditionally once `--rater` is given). The only file it ever writes is the sprint evidence JSON, and only under `--rater human`.

## How it coexists with the detection sheet

- **Never one sheet, two questions.** `make_rating_sheet.py --mode` is mutually
  exclusive — a single `rating_sheet.csv` is either a detection sheet or a preference
  sheet, enforced by the `_mode` marker in `answer_key.json` and by
  `score_preference.py` refusing to run against a key file that lacks
  `_mode: "preference"` (fail loud on a wrong-tool-for-the-job mistake rather than
  silently mis-scoring, matching `.claude/rules/ground-truth-over-proxy-signals.md`'s
  spirit of not trusting an unverified artifact).
- **Separate gate surfaces.** Detection continues to be the only signal
  `~/.human/blind_ab_gate.json` / `docs/evaluation/blind_ab_gate.json` and the C LoRA
  promotion code ever see. Preference's win-rate evidence lives only under
  `sprints/sprint-better-than-human-2026-09-05/evidence/US-6/` — a sibling measurement,
  not a competing gate. This matches `stories.md:322-323`'s explicit scope statement
  and avoids the exact failure class `blind_ab_gate.py:53-59`'s docstring documents
  (two different rating regimes colliding on one promotion-authoritative key).
- **Shared upstream, forked downstream.** Steps 1-2 (`export_seth_triples.py`,
  `gen_huuman_replies.py`) and the redaction/shuffle mechanics in
  `make_rating_sheet.py` are shared code paths; only the question framing, the key's
  semantic meaning, and the scoring/report/refusal layer diverge. This is the
  "extend, don't duplicate" instinct from `~/.claude/rules/writing-rules.md` applied to
  code instead of rules.

## Score.py's n=0 refusal — precedent, reused not reinvented

`score.py:307-315`'s guard is the direct ancestor of refusal condition #1 above:

```python
if agg["n"] == 0:
    print("RESULT_blind_ab=INVALID (0 items scored — no choices matched the "
          "key; refusing to emit any verdict or gate file)", file=sys.stderr)
    sys.exit(3)
```

This exists because of the 2026-07-25 incident (`.claude/rules/no-number-without-a-measurement.md`):
an unjudged sheet scored "PASS n=0" and clobbered the gate with a vacuous green. The
preference path inherits the identical hazard shape — `wilson(0, 0)` returns
`(0.0, 0.0, 0.0)` (`score.py:31`), which is a *well-formed* number that satisfies "win
rate ≥ 0.5" exactly as often as it satisfies "win rate < 0.5", i.e., never
distinguishes anything. `score_preference.py` therefore checks `agg["n"] == 0` **before**
computing or printing `win_rate` at all (not just before the n<20 gate — n=0 is caught
by n<20 too, but the explicit n==0 check is kept as belt-and-suspenders because it is
the exact shape of the historical incident, and a reviewer checking for the regression
should find the same-shaped guard, not an equivalent-but-differently-written one).

## Hermetic tests

Following `test_score.py:312-364`'s established pattern (`tempfile.TemporaryDirectory`,
`HOME` env override, `subprocess.run` against the real script — not mocked internals):

- `test_make_rating_sheet.py`:
  - `redact()` strips a phone-shaped string (`"call me 555-123-4567"` → contains
    `[phone]`, not the digits) and a fixture-injected name (`"tell Sarah hi"` →
    `[name]`, not `"Sarah"`) — both assert the *raw* string is absent from output, not
    just that a replacement happened (a test asserting "output != input" would pass
    even if redaction picked the wrong span).
  - `--mode preference` produces an `answer_key.json` with `_mode: "preference"` and
    per-id values that are still exactly `"A"`/`"B"`.
  - A triple with `seth_reply == huuman_reply` (post-redaction) is excluded from
    `rating_sheet.csv`, and the skip count is printed.
  - All-duplicate input → exit non-zero, no `rating_sheet.csv` or `answer_key.json`
    written (checked via `os.path.exists`).
- `score_preference.py` tests (in `test_score.py`, alongside `TestRaterGateSeparation`,
  or a new `TestPreferenceScoring` class in the same file):
  - n=15 (< 20) → exit non-zero, no evidence file.
  - `--rater synthetic` → prints win rate, writes no evidence file under
    `sprints/.../evidence/`.
  - A `judge_api`-stamped sheet with `--rater human` → refused (reuse
    `detect_rater_kind` fixture shape from `test_score.py:410-424`).
  - A key file missing `_mode: "preference"` → refused with a clear message (wrong
    tool for this key).
  - No network, no `~/.human` writes outside the `HOME`-overridden tempdir, no LLM
    call anywhere in the test.

## Conflicts / cross-story dependencies

- **US-3** (`stories.md:99-119`) imports `wilson()` from `score.py` read-only. This
  design does not touch `wilson()`'s signature or behavior — verified no edit is
  planned to `score.py` at all; `score_preference.py` only imports from it. No
  conflict.
- **No other story in this sprint touches `scripts/blind_ab/make_rating_sheet.py`,
  `score.py`, or `PROTOCOL.md`** (confirmed via `grep -n "blind_ab\|score\.py\|make_rating_sheet" stories.md`
  — only US-1/US-2/US-3/US-6 mention `scripts/blind_ab/` at all, and US-1/US-2 touch
  `authorship_gap.py`/corpus scripts, not these files). No merge risk within the
  sprint.
- **Sequencing**: US-6 has no blocking dependency (`stories.md:225`: "Dependencies:
  none"). It can run independently, but AC-6.5's real n≥20 run needs rater time — the
  same human-in-the-loop boundary the `blind-ab-pipeline` skill already documents
  ("Step 4 requires the user"). This story's implementation (code + hermetic tests) is
  agent-completable; the evidence artifact is not, and should be tracked as a
  follow-up chip if rater availability doesn't land inside this sprint's window (echoing
  the existing `evidence/US-4/RATING-BLOCKED.md` pattern referenced in the skill).

## Risks

- **Sheet/protocol conflation**: mitigated by the `_mode` marker + `score_preference.py`
  refusing a wrong-mode key, and by PROTOCOL.md's new section being visually and
  textually separated from the detection section (own heading, own instructions file).
- **PII leak into repo**: mitigated by redaction at build time (both modes) + the
  evidence JSON schema containing only `{n, win_rate, ci_lo, ci_hi, rater, date}` — no
  `context`/`seth_reply`/`huuman_reply` field is ever serialized into a committed file.
  Residual risk: redaction is regex+name-list based, not perfect (a nickname not in
  AddressBook, an unusual phone format) — this is a known limitation, not a silent gap,
  and should be stated in `PROTOCOL.md`'s new section so a human reviewer double-checks
  before sending a sheet to raters, same as the existing manual-recruitment step already
  requires human judgment.
- **Small-n CI too wide to conclude**: real; AC-6.6 already commits to reporting a
  win rate below 0.5 honestly rather than re-sampling — this design adds no retry loop,
  and the Wilson CI is reported alongside the point estimate specifically so "n=20,
  win_rate=0.55" is legible as "consistent with 0.5" rather than misread as a win.
- **Rater fatigue**: addressed by the design decision (rater-flow §4) to reuse the same
  exported triples rather than a second `chat.db` export, though the *time* cost of a
  second question pass per item is a genuine open question this design does not resolve
  (flagged in stories.md and restated above, not silently assumed away).

## Out of scope

- Changing the detection protocol, its 0.60/0.55/0.60 thresholds, or
  `compute_gate_verdict()` (`score.py:122-135`).
- Wiring preference win rate into any promotion gate (`blind_ab_gate.py`,
  `~/.human/blind_ab_gate.json`) — this is a measurement, not a gate, per
  `stories.md:322-323`.
- Building a new content-redaction system beyond regex phone matching + AddressBook
  name matching (e.g., NER-based PII scrubbing) — out of scope for an M-sized story and
  not requested by AC-6.4, which asks for phone-shaped and contact-name-shaped strings
  specifically.

## Estimate

**M**, matching `stories.md:226` and the skeleton — confirmed reasonable given the
implementation is almost entirely additive/reuse (one new flag + one `redact()`
function + one thin new script that imports rather than forks `score.py`'s math), with
the bulk of the M-sized effort in the two new hermetic test files and the real n≥20
rater run (human-in-the-loop, not agent time).

RESULT_tech-lead=READY — design grounded in a fully-read pipeline (`make_rating_sheet.py`, `score.py`, `blind_ab_gate.py`, `PROTOCOL.md`, `export_seth_triples.py`, `gen_classifier_trials.py`, `mine_all_data.py`'s contact/phone helpers, the blind-ab-pipeline skill, and a listing-only check of `~/.human/blind_ab_human/`) with every claim verified by grep/read and file:line cited; no UNVERIFIED items remain.
