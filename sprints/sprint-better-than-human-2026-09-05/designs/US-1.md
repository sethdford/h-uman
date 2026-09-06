# Design for US-1: Rebalance and re-provenance the authorship preference corpus

**Status:** READY
**Sprint:** sprint-better-than-human-2026-09-05
**Worktree:** `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-better-than-human-2026-09-05`
**Story:** `sprints/sprint-better-than-human-2026-09-05/stories.md` US-1 (P0)

## 0. Two corrections to the story text, verified against the tree

Before the approach: AC-1.1 as written misattributes provenance facts from its own
cited source (`docs/plans/2026-09-02-persona-evolution/spec.md` §3b). Both are
verified by reading the spec table and counting the actual files on disk — not
inferred. Implementers should follow this design's corrected reading, not AC-1.1's
literal file/count pairing.

**Correction 1 — which file has the 59 new rows.** AC-1.1 says: "`~/.human/logs/eval-archive/ground_truth-backup-20260725-113527.jsonl`
(690 rows, 59 not already in the repo copy)." The spec's own table says the
opposite of this pairing:

| Store | Spec verdict (spec.md line) | Verified on disk today |
|---|---|---|
| `~/.human/logs/eval-archive/ground_truth-backup-20260725-113527.jsonl` | "usable; strict subset of `training_pairs` (**0** rows not in it)" (`spec.md:133`) | 690 lines, mtime 2026-07-25 |
| `eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl` | "**used** (**59** rows not in the repo copy)" (`spec.md:134`) | 1,302 lines, mtime 2026-07-25 |
| `data/imessage/training_pairs.jsonl` (repo, gitignored, `.gitignore:141`) | "**best — used**" (`spec.md:137`) | 1,303 lines, mtime 2026-07-26 — **absent from this worktree** (see Correction 2) |

The spec's own "Chosen second store" line (`spec.md:146`) names `data/imessage/training_pairs.jsonl`
plus "the 2026-07-25 backup of the same export" — i.e. the **`imessage-corpus-backup`**
file, not `ground_truth-backup`. The spec's own re-measurement narrative confirms
the same pairing: "repo export 1,303 added; backup 1,302 → 59 added / 1,243
duplicates" (`spec.md:180-181`). `ground_truth-backup` is never the file credited
with the 59 new rows anywhere in the spec — AC-1.1 conflated two different backup
files that happen to share a directory and a date stamp.

**Why this matters:** if a merge literally followed AC-1.1's stated pairing
(`training_pairs.jsonl` + `ground_truth-backup`), the spec's own numbers predict it
adds **zero** new rows over `training_pairs.jsonl` alone — directly undermining the
story's stated goal ("expanded with additional Seth-authored... text"). This design
merges all three "used"/"usable" stores (`training_pairs.jsonl`, the
`imessage-corpus-backup` `training_pairs.jsonl`, and `ground_truth-backup`) so the
actual yield is measured rather than assumed either way, and reports each source's
`rows/added/duplicates` individually (§4) — satisfying AC-1.1's own instruction to
print the actual count rather than assume a citation.

**Correction 2 — the primary source doesn't exist in this worktree.** `data/imessage/`
is gitignored (`.gitignore:141`) and is not tracked by git, so a `git worktree`
checkout does not materialize it — confirmed: `data/imessage/` does not exist
anywhere under this worktree, while it exists at
`/Users/sethford/Projects/h-uman/data/imessage/training_pairs.jsonl` (main checkout,
1,303 lines, matches the spec's count). The merge script (§2) therefore takes
explicit, **required** `--primary`/`--extra` path arguments with no baked-in
repo-relative default — see Risks (§6) for the exact invocation to use from this
worktree.

## 1. Approach

Add one new, dependency-free Python script,
`scripts/merge_seth_preference_sources.py`, that:

1. Reads the three provenance-verified Seth-authored stores named in §0 (one
   `--primary`, N `--extra`), validating every row's shape as it goes (Seth-authorship
   assertion — AC-1.2), and de-dupes them against each other using the **exact**
   `(timestamp-to-the-second, sha256(stripped text))` key already implemented at
   `scripts/eval_persona_evolution.py:475-479` (`dedup_key`) — imported, not
   re-derived, per this repo's own convention (`rebalance_preference_corpus.py`'s
   docstring: "a second hand-rolled definition... is exactly the kind of drift that
   caused the 2026-07 deliberation-leak diagnosis").
2. Emits the merged, deduplicated Seth texts as **KTO-shape** rows
   (`{"prompt": ..., "completion": <seth_text>, "label": true}`) — see §2 for why KTO,
   not a paired `{prompt,chosen,rejected}` shape.
3. Reads an existing preference corpus's `rejected` field
   (`~/.human/training-data/glm-v61-pref/train.jsonl`, unmodified, read-only) and
   re-emits those texts, **with their own original prompts**, as KTO
   `label: false` rows — supplying the negative side no model call is needed to
   produce.
4. Writes `train.jsonl` + `manifest.json` to a new, explicit `--out-dir` under
   `~/.human/training-data/` (gitignored; AC-1.5), and prints its own actual
   per-source `added`/`duplicates` counts (AC-1.1) plus label counts.
5. Refuses (exit non-zero, writes nothing) on any of the conditions in §4.

The output is then run once through the **existing, unmodified**
`scripts/rebalance_preference_corpus.py --match-sides` (AC-1.3/1.4 — its refusal
contract already matches AC-1.4 verbatim; verified in §3). No C changes. No model
load anywhere in this pipeline (verified in §5).

## 2. Key design decision: KTO shape, not paired DPO/ORPO shape — and why

`scripts/rebalance_preference_corpus.py` accepts exactly two row shapes
(`index_rows`, `rebalance_preference_corpus.py:291-317`):
- preference/ORPO/DPO: `{"prompt","chosen","rejected"}` — **same row**, both sides
  paired to one prompt.
- KTO: `{"prompt","completion","label"}` — chosen (`label=true`) and rejected
  (`label=false`) are **different, unpaired** rows.

Neither `data/imessage/training_pairs.jsonl` nor the `ground_truth`/
`imessage-corpus-backup` exports contain a competing ("rejected") reply for any
given context — confirmed by inspecting one row's **keys only** from each (no
message text read):
- `training_pairs.jsonl` row: `{"messages":[...], "metadata":{"chat_id","reply_length","timestamp"}}`,
  `messages[-1] == {"role":"assistant","content": <seth_text>}` per
  `scripts/extract_imessage_pairs.py:256-267` (`extract_training_pairs`) — the SFT
  chat-window format, one side only.
- `ground_truth-backup` row: `{"incoming","seth_reply","context_turns","delay_seconds",
  "chat_id","timestamp","hour_of_day","day_of_week"}` per
  `extract_imessage_pairs.py:277-315` (`extract_ground_truth`) — again one side only.

Producing a genuine competing reply for these contexts would require a live model
call — explicitly forbidden by AC-1.6 and `.claude/rules/no-two-model-loaders`-class
constraints (`scripts/check-no-resident-model.sh`). **Rejected alternative:** pair
each new Seth text with the base corpus's rejected texts *by prompt match* (DPO
shape) — impossible without fabricating a prompt correspondence that doesn't exist,
which would silently manufacture data (`.claude/rules/reports-success-does-nothing.md`).
**Rejected alternative 2:** import the private `_export_record_seth_text`
(`eval_persona_evolution.py:438-451`, leading underscore) to get a prompt for free —
rejected because (a) it discards context on purpose for its own use case
(style-axis measurement doesn't need a prompt) and (b) US-7 also edits
`eval_persona_evolution.py` this sprint (§7); depending on a private symbol across
that edit is fragile. This design duplicates ~10 lines of shape validation instead
(the SAME two `if` branches `_export_record_seth_text` uses) rather than share a
private function or touch a contested file.

KTO is the only shape that fits without inventing data: each new chosen row keeps
its **own real prompt** (the actual incoming context), each reused rejected row
keeps **its own real (different) prompt** — by design, KTO training doesn't require
prompt correspondence between labels. `rebalance_preference_corpus.py`'s own test
suite already exercises this exact shape end-to-end, including with `--match-sides`
(`scripts/test_rebalance_preference_corpus.py:100,290,333,428` — `_kto_rows` helper,
`test_index_rows_kto_shape`, `test_cli_handles_kto_shape`,
`test_match_sides_kto_labels_get_same_distribution`), so no change to
`rebalance_preference_corpus.py` is needed (verified: `main` HEAD `b24fb8e50` ==
this worktree's copy, byte-identical `diff`, checked 2026-09-05).

**Scope note, stated explicitly so it isn't assumed either way:** this design does
**not** claim the merged+rebalanced output becomes the literal input to the next
mlx-tune training cycle — that would be a deliberate, separate decision (AC-1.6:
"does not trigger a training run"). See Risks §6 for the live, uncommitted
`glm-v61-pref-casing-20260905-085655/` snapshot already staged against the
*old*, un-merged v61 corpus, and the coordination this implies.

## 3. Files touched

| File | Change | Why |
|---|---|---|
| `scripts/merge_seth_preference_sources.py` (new) | Merge + provenance assertion + KTO emission + manifest | Core of this story |
| `scripts/test_merge_seth_preference_sources.py` (new) | Hermetic tests (§8) | AC-1.1/1.2/1.5 pinned |
| `.github/workflows/ci.yml` | One line added to the existing `capability-gate-check` job (`ci.yml:953-971`), alongside `test_rebalance_preference_corpus.py`'s sibling tests | So the new test actually runs somewhere — `test_rebalance_preference_corpus.py` itself is stdlib-only and already fits this job's constraints (verified: no torch/mlx import in either script) |
| `sprints/sprint-better-than-human-2026-09-05/evidence/us1-merge-manifest.json` | Committed: per-source counts, label counts, no message text | AC-1.1 evidence |
| `sprints/sprint-better-than-human-2026-09-05/evidence/us1-rebalance-stats.json` | Committed: copy of `rebalance_preference_corpus.py`'s own sidecar (before/after margins) | AC-1.3 evidence |

**Not touched:** `scripts/build_v6_preference_corpus.py` (no changes needed — this
story adds a parallel script rather than extending it, since its existing sources
are unrelated dpo_pairs/cycle4 pools, not the two-export merge this story asks for).
`scripts/rebalance_preference_corpus.py` (verified byte-identical to `main`;
its contract already satisfies AC-1.3/1.4 with zero changes). `scripts/eval_persona_evolution.py`
(deliberately not touched — see §2 and §7 conflict note).

## 4. Data sources, provenance, and refusal conditions (merge script)

Chosen/label=true pool — must satisfy ALL of:

| Source | Path (must be passed explicitly, see §6) | Spec verdict | Rows (measured 2026-09-05) |
|---|---|---|---|
| `--primary` | `data/imessage/training_pairs.jsonl` (main checkout; absent in this worktree) | "best — used" (`spec.md:137`) | 1,303 |
| `--extra` | `~/.human/logs/eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl` | "used" (`spec.md:134`) | 1,302 |
| `--extra` | `~/.human/logs/eval-archive/ground_truth-backup-20260725-113527.jsonl` | "usable" (`spec.md:133`) | 690 |

Every row is shape-validated on read (mirrors `_export_record_seth_text`'s two
accepted shapes, `eval_persona_evolution.py:438-451`, duplicated locally per §2):
`messages[-1].role == "assistant"` for the `training_pairs` shape, or
`seth_reply`+`timestamp` present for the `ground_truth` shape. **Any row matching
neither shape is FATAL** (exit non-zero, nothing written, message names the file
and line) — this is the AC-1.2 "excluded... verified by a script assertion, not
manual review" contract: a `memory.db`/`dpo_pairs`/`production_outcomes` row
literally cannot pass this check (those stores don't have `messages`+`metadata.timestamp`
or `seth_reply`+`timestamp` — confirmed by their schemas in
`docs/plans/2026-09-02-persona-evolution/spec.md:129-131`), so admission is
structural, not a promise. Empty/whitespace-only texts and tapback echoes are
dropped (reusing the `TAPBACK_PREFIXES` constant, `eval_persona_evolution.py:114`,
imported not re-derived), matching `read_export_jsonl`'s own filtering.

De-dup: exact `dedup_key` import (`eval_persona_evolution.py:475-479`), applied
across ALL three sources in the order primary → extra₁ → extra₂ (first-seen wins,
matching `merge_sources`'s own convention, `eval_persona_evolution.py:482-499`,
whose reporting shape — `{"path","rows","added","duplicates"}` — this script's
manifest reuses verbatim). No timezone conversion is needed (unlike
`eval_persona_evolution.py`'s chat.db merge): all three sources were written by the
same `extract_imessage_pairs.py` local-naive convention, so comparing their raw
ISO timestamps is already apples-to-apples.

Rejected/label=false pool:

| Source | Path | Provenance requirement |
|---|---|---|
| `--rejected-pool` | `~/.human/training-data/glm-v61-pref/train.jsonl` (read-only; NOT the `-casing-20260905-085655` rebalanced snapshot — see Risks §6) | None — rejected is by construction never Seth-authored; AC-1.2 only constrains the chosen side |

**Merge-script refusal conditions** (exit non-zero, writes nothing):
1. `--primary`, any `--extra`, or `--rejected-pool` path does not exist or is not
   readable.
2. Any source row fails the two-shape check (§4, per-row FATAL).
3. Merged, deduplicated chosen (label=true) pool has fewer than `--floor` rows
   (default 500 — well below the ~1,300+ expected yield, so the floor only fires on
   a genuine input regression, matching `build_v6_preference_corpus.py`'s own
   `--floor` convention, `build_v6_preference_corpus.py:264-265,251-254`).
4. `--rejected-pool` contains zero rows with a non-empty `rejected` field.

**Downstream refusal (unchanged, `rebalance_preference_corpus.py`, verified
byte-identical to `main`):** no readable style card / CLI target (AC-1.4a,
`rebalance_preference_corpus.py:349-388`); zero rebalanceable rows or (with
`--match-sides`) zero rejected rows (`:515-524`); post-rebalance margin on either
axis exceeds `--max-margin` (default 0.10) with `--match-sides`
(`:553-565`). All pinned by the EXISTING test suite
(`scripts/test_rebalance_preference_corpus.py`), re-run as a regression check, not
re-implemented.

## 5. Privacy handling

- No message text, phone numbers, or contact names are read in this design
  process (verified above by printing only `.keys()` / line counts, never message
  bodies) and none will be written to the repo by the implementation.
- Merge output (`train.jsonl`, `manifest.json`) lives under
  `~/.human/training-data/<out-dir>/` — outside the repo entirely, already
  gitignored by virtue of location (not a repo path, so `.gitignore` doesn't even
  need to cover it — unlike `data/imessage/`, which needs its `.gitignore:141`
  entry precisely because it IS a repo path).
- Only the two committed evidence files (§3) — aggregate counts, source paths (not
  file *contents*), and margin numbers — go in the repo. No `--primary`/`--extra`/
  `--rejected-pool` file's contents are ever printed to stdout or a log; only counts.
- No model load, no network call anywhere in this pipeline (`grep -n "^import\|^from"`
  on both `rebalance_preference_corpus.py` and `eval_persona_evolution.py`: stdlib +
  each other only — no `torch`/`transformers`/`mlx`/`requests`, verified 2026-09-05).

## 6. Risks

- **`check-no-resident-model.sh` / two-model-loader rule:** N/A by construction — no
  model is loaded anywhere in the merge or rebalance path (verified §5). Nothing to
  gate.
- **`:8741` / service-loop:** untouched; this story reads only local files.
- **File-size/sqlite/clone/agent-core ratchets:** N/A — zero C files touched.
- **Worktree materialization gap (verified, §0 Correction 2):** `data/imessage/`
  does not exist in this worktree. The implementer must invoke the merge script
  with **absolute** paths into the main checkout, e.g.:
  ```
  python3 scripts/merge_seth_preference_sources.py \
    --primary /Users/sethford/Projects/h-uman/data/imessage/training_pairs.jsonl \
    --extra ~/.human/logs/eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl \
    --extra ~/.human/logs/eval-archive/ground_truth-backup-20260725-113527.jsonl \
    --rejected-pool ~/.human/training-data/glm-v61-pref/train.jsonl \
    --out-dir ~/.human/training-data/glm-v6-merged-20260905/
  ```
  (per `.claude/rules/worktree-cwd-resets-in-bash.md` — every Bash call is a fresh
  shell, so this must be a single absolute-path invocation, not a `cd`-then-run).
- **A live, uncommitted `--match-emoji` extension to `rebalance_preference_corpus.py`
  exists outside version control (verified, not inferred):**
  `~/.human/training-data/glm-v61-pref-casing-20260905-085655/train.rebalance_stats.json`
  (written TODAY, 08:56) records `"match_emoji": true` and a full `emoji_rate` stats
  block — a CLI flag and stats field that do **not exist** in
  `scripts/rebalance_preference_corpus.py` on `main` (`b24fb8e50`) or in this
  worktree (confirmed byte-identical `diff`). Some other, non-sprint session ran a
  locally-modified, never-committed copy of this script. Two implications for
  whoever implements US-1: (a) don't assume the CLI surface documented in §3/§4 is
  final — re-check `git status`/`git diff` on `rebalance_preference_corpus.py`
  immediately before running it, in case that extension has since been committed;
  (b) that snapshot dir is evidence a **separate, already-completed** rebalance ran
  today directly on the *old*, un-merged `glm-v61-pref/train.jsonl` (426/426 rows,
  margins 0.5423→0.0047 lowercase / 0.2653→0.0 terminal-punct — close to but not
  identical to the closing report's cited 0.715→0.005/0.338→0.000, which came from
  a different corpus, likely `glm-v6-pref` at 415 rows, not `glm-v61-pref` at 463).
  **This story's merge is additive to, not a replacement for, that snapshot** — it
  does not need to be deleted or reconciled, but whoever wires the *next* actual
  training run's `config.yaml` needs to decide (out of scope here, AC-1.6) whether
  it points at `glm-v61-pref-casing-20260905-085655/` (today's un-merged rebalance)
  or a fresh rebalance of this story's merged corpus. Flag this explicitly to the
  scrum master / stakeholder rather than silently picking one.
- **Casing/punct axis functions could change under this story's feet:** US-5's
  register boundary (`.claude/rules` cross-reference, AC-5.2) reuses a *different*
  constant (`authorship_gap.py:87`'s inline `<=12` words), not
  `eval_persona_evolution.py`'s casing axes — no actual collision, checked.
  `eval_persona_evolution.py`'s `starts_lowercase`/`terminal_punctuation` (imported
  by `rebalance_preference_corpus.py`) are within US-7's edit scope this sprint
  (§7). If US-7 lands first and changes either function's behavior, re-run (not
  re-design) this story's rebalance step before trusting its margins.

## 7. Sequencing / conflicts with other stories

Read "Files likely touched" for all 8 stories (`stories.md`):

| Story | Files | Overlap with US-1 |
|---|---|---|
| US-2 | `scripts/blind_ab/authorship_gap.py`, `scripts/register_v6_adapter.py`, new `scripts/test_authorship_promotion_gate.py` | None. Soft dependency (US-2 *on* US-1, not file-level) — US-2's gate is most useful scored against whatever adapter eventually trains on this story's corpus, but nothing here blocks US-2's own build+fixture-test work. |
| US-3 | new `scripts/eval_seth_initiation_baseline.py`, new test | None |
| US-4 | `scripts/eval_when_to_speak.py` (maybe) | None |
| US-5 | `src/memory/semantic_recall.c`, `include/human/memory/semantic_recall.h`, `src/memory/retrieval/hybrid.c`, `scripts/eval_semantic_live_gate.py`, new `tests/test_semantic_recall_register.c` | None (C-only + a different eval script) |
| US-6 | `scripts/blind_ab/make_rating_sheet.py`, `scripts/blind_ab/score.py`, `PROTOCOL.md`, `test_score.py` | None |
| US-7 | `scripts/eval_persona_evolution.py`, `scripts/test_eval_persona_evolution.py`, `docs/plans/2026-09-02-persona-evolution/spec.md` | **Read-dependency, not a file conflict.** US-1 imports `dedup_key` and `TAPBACK_PREFIXES` (stable, unlikely to move) from this file but does not edit it, so no merge conflict. If US-7 lands first, its `--window-days` addition is additive (new function, new CLI mode) and shouldn't touch `dedup_key`/`TAPBACK_PREFIXES`/`starts_lowercase`/`terminal_punctuation` — worth a one-line confirmation from whoever implements US-7 rather than assuming. |
| US-8 | `src/agent/model_router.c`, new eval script, new C test | None |

**No file-level conflicts with any of the other 7 stories.** US-1 can start and
finish independently; it only *feeds* US-2 (data, not code). Recommended order:
land US-1 before or in parallel with US-2 (not required), since US-2's fixture test
(AC-2.4, reconstructing the known regression shape) doesn't need US-1's output to
be written first.

## 8. Hermetic test plan (`scripts/test_merge_seth_preference_sources.py`)

Model: `scripts/test_rebalance_preference_corpus.py` (subprocess-driven CLI tests
over `tempfile` JSONL fixtures, `_write_jsonl`/`_run` helpers) — same pattern, same
stdlib-only dependency set (`json`, `os`, `subprocess`, `sys`, `tempfile`; no
`pytest` requirement beyond what's already vendored for the sibling test, runnable
via `python3 scripts/test_merge_seth_preference_sources.py` directly per the
`ci.yml:953-971` job's own invocation style).

All assertions are **non-vacuous** — each test's fixture is built so the assertion
fails if the behavior it names is absent (`.claude/rules/reports-success-does-nothing.md`,
`.claude/rules/tests-that-pin-bugs.md`):

1. `test_training_pairs_shape_parses_and_extracts_assistant_turn` — one synthetic
   `{"messages":[...],"metadata":{"timestamp":...}}` row with a KNOWN
   `messages[-1].content` string → assert the emitted KTO row's `completion`
   equals that exact string (not just "some row was produced").
2. `test_ground_truth_shape_parses` — one synthetic `{"seth_reply","timestamp",...}`
   row → same non-vacuous content-equality assertion.
3. `test_daemon_shaped_row_is_fatal` — a synthetic row shaped like `memory.db`
   `dpo_pairs` (`{"prompt","chosen","rejected","source":"outbound_edit"}`, no
   `metadata.timestamp`, no `seth_reply`) → assert **non-zero exit** and **no output
   file written** (the AC-1.2 contract; a test that only checked "doesn't crash"
   would pass on a silent skip, which is the exact anti-pattern this story exists to
   avoid on the CHOSEN side).
4. `test_dedup_key_collision_across_two_sources_drops_the_duplicate` — construct two
   fixture files sharing one row with the SAME `(timestamp-to-the-second,
   sha256(text))` but arriving via `--primary` and `--extra` respectively → assert
   the merged output contains exactly one copy, and the manifest's `--extra`
   entry reports `duplicates: 1` (not just "no crash on duplicate input").
5. `test_dedup_key_near_miss_is_not_deduped` — same text, timestamp differing by
   2 seconds → assert BOTH rows survive (proves the key is second-precision, not
   over-eager — guards against a future "helpful" widening of the window).
6. `test_rejected_pool_empty_is_fatal` — `--rejected-pool` file with zero rows →
   non-zero exit, nothing written.
7. `test_below_floor_is_fatal` — merged pool of 3 rows against `--floor 500` →
   non-zero exit, nothing written; exit message names the actual count and the
   floor (so a human reading CI output sees `3 < 500`, not a generic failure).
8. `test_output_is_valid_kto_shape_for_rebalance_script` — end-to-end: run the
   merge script's output straight into `rebalance_preference_corpus.py --dry-run`
   (imported as a module, matching `test_rebalance_preference_corpus.py:18`'s own
   `import rebalance_preference_corpus as rbc`) and assert it does NOT refuse — the
   contract-compatibility check between the two scripts, proven by actually running
   the second script against the first's output, not by asserting they merely agree
   on a schema diagram.
9. `test_no_raw_text_in_manifest` — manifest.json's JSON-serialized form must not
   contain any of the fixture's known input strings verbatim beyond what's expected
   (counts/paths only) — guards AC-1.5 mechanically rather than by convention.
10. `test_tapback_and_empty_rows_are_dropped_not_counted` — a fixture row whose
    `seth_reply` is a tapback-prefixed string (reuses the real
    `TAPBACK_PREFIXES` values via import, not a hand-copied guess) → assert it does
    NOT appear in the output and IS counted separately from "duplicates" in the
    manifest (so a human auditing the manifest can tell "dropped as noise" from
    "dropped as duplicate").

## 9. Measurement + refusal (this story's own evidence)

Two numbers get committed to `sprints/sprint-better-than-human-2026-09-05/evidence/`,
each sourced from a script's own JSON output — never hand-typed:

- **Merge counts** (`us1-merge-manifest.json`): total rows per source, `added`/
  `duplicates` per `--extra`, final chosen (label=true) and rejected (label=false)
  row counts. Refuses per §4 rather than reporting a partial/guessed count.
- **Rebalance margins** (`us1-rebalance-stats.json`): copy of
  `rebalance_preference_corpus.py`'s own `--sidecar` output — before/after
  `lowercase_start_rate` and `terminal_punct_rate` margins for THIS run's merged
  corpus (AC-1.3). Per `.claude/rules/no-number-without-a-measurement.md`: if the
  post-rebalance margin exceeds `--max-margin` on either axis, the refusal (§4,
  downstream) fires and **no** margin number is committed as if it were a pass —
  the failing run's exact refusal message is what gets recorded in that case,
  not a fabricated or omitted number.

## 10. Estimate

**M** (matches the story's own sizing). Breakdown: merge script + manifest ~120
LOC (smaller than `build_v6_preference_corpus.py`'s 314 LOC — no dpo_pairs/cycle4
sourcing logic to replicate); test file ~150-200 LOC across the 10 cases in §8
(comparable density to `test_rebalance_preference_corpus.py`'s 549 LOC for a
harder, two-schema, two-flag-mode script); one CI line; two evidence JSON commits
after a real run. No C, no model load, no new ratchet interactions — the estimate
risk is entirely in getting the KTO-shape reasoning (§2) right the first time,
which this design has already resolved.

RESULT_tech-lead=READY — approach, file:line-cited touch points, provenance,
privacy handling, refusal conditions, hermetic non-vacuous test plan, and
cross-story sequencing are verified against the current tree (not assumed); two
factual corrections to the story text (§0) and one live external risk (§6,
uncommitted `--match-emoji` extension) are flagged for the scrum master rather
than silently resolved.
