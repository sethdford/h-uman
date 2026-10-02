---
title: Learned Style Profile — nightly learner, drift check and runtime gate
created: 2026-10-01
status: operator-facing
---

# Learned Style Profile

Hand-written persona style rules ("5-15 words", "MAX 15 words", "Be brief")
are replaced by values **learned from Seth's own sent iMessages**, per contact
and per situation. They adapt over time and are self-checked every night.

Two parts share one file:

| Part | What | Where |
|---|---|---|
| A (Python) | nightly learner + drift self-check | `scripts/learned_style_profile.py`, `scripts/learned_style_drift.py`, `scripts/learned_style_nightly.sh` |
| B (C) | runtime loader, lookup, prompt line | see [Runtime (Part B)](#runtime-part-b) |

The hand-written persona JSON is **never modified**. On 2026-09-06 an
automatic style re-analysis gutted the persona, so the learned values live in
a separate file that the runtime reads on top of the persona.

## Gates

| Variable | Values | Default | Effect |
|---|---|---|---|
| `HU_LEARNED_STYLE_LEARN` | `0` or anything else | on | `0` makes the nightly wrapper a no-op. Writing the file changes nothing that is sent: only Part B reads it, behind its own gate. |
| `HU_LEARNED_STYLE` | `off` / `shadow` / `live` | `off` | Part B's runtime gate (below). |

## The file

`~/.human/personas/<persona>.learned-style.json`, mode 0600, written atomically
(temp file + rename). The persona dir is `$HU_PERSONA_DIR`, else
`$HU_STATE_DIR/personas`, else `~/.human/personas`, the same order as
`hu_persona_base_dir`. memory.db and the logs dir default under
`$HU_STATE_DIR` (or `~/.human`) too. Schema `learned-style/v1`:

```
{
  "schema": "learned-style/v1", "persona": "seth",
  "generated_at": "2026-10-02T05:10:00Z", "window_days": 180, "half_life_days": 21,
  "global": { stats },
  "contacts": { "<handle>": { "overall": { stats },
                              "buckets": { "shape:question": { stats }, ... } } }
}
```

`stats` holds `n`, `n_eff`, `len_p25` / `len_p50` / `len_p90` (reply length in
UTF-8 **bytes**, summed over the turn's bubbles),
`bubbles_p50`, `lower_start_rate`, `emoji_rate`, `end_punct_rate`,
`latency_p50_s` (or null) and `shrunk`. Every leaf is a number, a boolean or
null, except the three metadata strings `schema`, `persona` and
`generated_at`. A test walks every leaf of a generated file and fails on
anything else.

Buckets: `shape:question|story|casual` (shape of the inbound burst being
answered: every bubble the contact sent since Seth's previous send, in time
order, joined with `\n`; Part B classifies the same joined text), `time:day|evening|late` (local time of the reply), `pace:rapid`
(inbound under 120 s after Seth's previous send, and a reply within 120 s).
Part B only reads the `shape:*` buckets in v1.

## Learner (Part A)

```bash
python3 scripts/learned_style_profile.py --persona seth --dry-run   # counts only, writes nothing
python3 scripts/learned_style_profile.py --persona seth             # write
python3 scripts/learned_style_profile.py --persona seth --no-cap    # reseed, skip the change cap
```

Overrides (used by the tests): `--persona-dir`, `--out-dir`, `--chat-db`,
`--memory-db`, `--log-dir`, `--now`, `--tz utc`. `--max-ambiguous-frac`
(default 0.05) sets the attribution refusal below.

**What is a sample.** A reply *turn* is one or more consecutive `is_from_me`
bubbles, each within 90 s of the one before. It is a sample when the message
immediately before it is the contact's, sent at most 6 hours earlier, in a
1:1 chat with a persona contact. That inbound message is the one it answers.
A Seth follow-up with no inbound message in between is not a reply and is
skipped. Length is the sum of the bubbles' UTF-8 bytes; attachment-only turns
are skipped.

**Only Seth's own texts.** chat.db marks the daemon's sends `is_from_me` too.
Attribution reuses `eval_conversation_quality.attribute()` (memory.db
`outbound_sends` provenance plus the assistant-row match). A turn with any
h-uman or ambiguous bubble is dropped. Learning from the twin's own sends
would feed its habits back into its style. For that reason memory.db is
required: if it cannot be read, the learner refuses. Owner self-test handles
(persona contact `relationship: "test"`) are excluded too.

**The attribution's blind spot, and its guard.** Before the first
`outbound_sends` record, a send with no assistant row within 15 minutes is
labelled Seth's; after it, any send provenance does not claim is labelled
Seth's. A twin reply from a path that writes neither would therefore be
learned as Seth's, and the drift check would not see it, because it uses the
same attribution. Every run logs `ambiguous_n`, `sent_n`, `ambiguous_frac`,
`huuman_n` and `exact_unmatched` (outbound_sends records that never resolved
to a delivered message), and the learner refuses when `ambiguous_frac`
exceeds `--max-ambiguous-frac`. `ambiguous` (h-uman was active nearby but the
delivered text matches nothing it logged) is the observable symptom of that
leak; a rising `exact_unmatched` says provenance and chat.db have drifted
apart.

**Privacy.** Message text exists only in memory inside
`samples_from_timeline()`, where it is reduced to numbers. Nothing with text
is written, logged or printed. The run log and stdout carry counts only, never
a handle.

**Shape rule** (identical in Part B, same test vectors in both suites),
applied to the joined inbound burst. Trim C whitespace, then: `question` if the text contains `?`; else `story` if it is
at least 140 bytes, or at least 80 bytes with at least 2 runs of `.`/`!`;
else `casual`. Empty or missing text is `casual`.

**Weighting and shrinkage.** Recency weight `0.5 ** (age_days / 21)` over a
180-day window; `n_eff` is the sum of weights. Quantiles are weighted (the
smallest value whose cumulative weight reaches q). Every value field is
shrunk toward its parent, `(n_eff * v + 8 * parent) / (n_eff + 8)`: bucket →
contact overall (itself shrunk) → global. `n` and `n_eff` are never shrunk.
`shrunk` is true on every contact and bucket entry (the formula always
applies) and false on `global`.

**Thresholds.** A bucket with `n < 3` is omitted; a contact with `n < 5` is
omitted, and Part B falls back to global for them.

### Guardrails

- **Per-run change cap.** Each value may move at most `max(30% of the previous
  value, an absolute floor)` per run, compared field by field with the same
  node in the previous file. The floor is 10 bytes for lengths (contract),
  0.05 for rates, 0.5 for `bubbles_p50` and 60 s for latency. The last three
  are not in the contract: without a floor, a rate that was 0.0 could never
  move again. A clamped value is counted in the log line (`clamped_n`,
  `clamped_fields`). There is no cap on the first run or with `--no-cap`.
  After clamping the quantiles are kept ordered (p25 ≤ p50 ≤ p90) by lowering
  the higher one, never by raising p50 or p90 past their own cap. With an
  ordered previous file this never triggers; it guards a hand-edited one.
- **History.** Before each write the previous file is copied to
  `~/.human/personas/learned-style-history/<persona>.<UTC stamp>.json` (0600);
  the newest 14 are kept.
- **Run log.** One JSON line per run in `~/.human/logs/learned-style.jsonl`:
  `status`, `samples`, `contacts`, `contacts_omitted`, `buckets`, `global_n`,
  `clamped_n`, `clamped_fields`, `max_rel_change` (largest pre-clamp relative
  move), `first_run`, and the attribution counts above. Counts only.
- **Refusal (exit 2, nothing written, previous file untouched).** More than
  `--max-ambiguous-frac` (5%) of sends ambiguously attributed; global
  `n < 50`; more than 50% of the previous file's contacts would disappear;
  persona file, chat.db or memory.db unreadable. A refusal still appends its
  log line (not under `--dry-run`).
- **`--dry-run`** prints one counts-only JSON line, writes nothing (no file,
  history or log), and exits 2 when a real run would refuse.

## Drift self-check (Part A)

```bash
python3 scripts/learned_style_drift.py --persona seth
```

Compares h-uman's own outbound reply lengths with Seth's, per contact:

- h-uman: memory.db `outbound_sends`, channel `imessage`, kind `text`/`reply`,
  last 30 days (`--days`). Only `length(cast(text as blob))` is selected; the
  text never reaches Python. Sends within 90 s are summed into one turn, the
  learner's own turn rule, so both sides measure the same unit.
- Seth: the learner's samples (same attribution, pairing and 180-day window),
  unweighted.

Per contact: two-sample KS statistic, both medians and their ratio
(h-uman / Seth), both n. A contact is **flagged** when both sides have
`n >= 20` and `KS > 0.35`. Report: `~/.human/logs/learned-style-drift-YYYYMMDD.json`
(0600), contacts as ordinals `c1, c2, ...`, never handles. Exit 0 no flags,
1 at least one flag, 2 a database was unreadable (nothing written). Flagging
only reports; it changes nothing.

## Operations

### Nightly (not installed by this PR)

`scripts/learned_style_nightly.sh` runs the learner then the drift check
(both always run), and is a no-op under `HU_LEARNED_STYLE_LEARN=0`. Exit:
the learner's code if non-zero, else the drift check's.

The existing 05:10 orchestrator is `~/Library/LaunchAgents/ai.human.insight-nightly.plist`
(the sleep-time curator chain). It is not tracked in the repo, so this PR does
not edit it. To install, the lead adds one step to its `ProgramArguments`
command, **after** `--retire-superseded` and **before** `--names`, joined with
`;` on both sides:

```
… insight_stream.py --retire-superseded --consistency-k 3; /bin/bash scripts/learned_style_nightly.sh; /opt/homebrew/bin/python3 scripts/insight_stream.py --names …
```

Why there: the step takes seconds and needs no model; the wide pass after
`--names` runs until its 07:30 deadline, so nothing placed after it would run.
With `;`, a learner refusal never blocks the curator steps. Back up the plist,
then `launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/ai.human.insight-nightly.plist`
and `launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.insight-nightly.plist`.

The launchd python must be able to read chat.db (Full Disk Access). Check the
first night: `tail -1 ~/.human/logs/learned-style.jsonl` must show
`"status": "written"`. A `refused_db_unreadable` line means the interpreter
lacks access.

### First run

1. `--dry-run` on real data; check `samples`, `contacts`, `global_n` and that
   no refusal flag is set.
2. Write once. There is no cap on the first run.
3. Read the drift report the next morning.

### Rollback

- Stop learning: `HU_LEARNED_STYLE_LEARN=0` in the plist environment, or remove
  the wrapper step from the plist.
- Restore a previous profile:
  `cp ~/.human/personas/learned-style-history/seth.<stamp>.json ~/.human/personas/seth.learned-style.json`
- Remove the profile entirely: delete `~/.human/personas/seth.learned-style.json`.
  Part B treats a missing file as absent, and the next run is a first run
  (no cap).
- Reseed after a deliberate change (e.g. the persona's contact list was cut by
  more than half, which the learner refuses): move the old file into the
  history directory, then run without it, or run with `--no-cap`.

### Tests

```bash
python3 -m pytest -q tests/test_learned_style_profile.py tests/test_learned_style_drift.py
```

Hermetic: synthetic chat.db / memory.db built in a temp dir, HOME pointed at
the temp dir, a fake interpreter for the wrapper. They run in CI in the
`capability-gate-check` job's pytest step.

## Runtime (Part B)

Unit note for the rendered line: "characters" in it (for example "usually
about 25 characters") are the learned `len_*` values, which are **bytes**. An
emoji counts as 4, an accented letter as 2.

<!-- Part B (C runtime: loader, lookup, prompt line, HU_LEARNED_STYLE gate,
     SHADOW→LIVE measurement and rollback) is documented here by its PR. -->

_To be written by the Part B PR._
