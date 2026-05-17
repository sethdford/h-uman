# Design for US-7.2: Mine DPO pairs from outbound-dedup corrections

Status: **READY (with one AC interpretation noted in §7)**
Sprint base: `13b89763`
Worktree: `/Users/sethford/projects/h-uman/.claude/worktrees/hardcore-goldwasser-af5a11`

---

## 0. Critical mismatches between the AC and the codebase (read this first)

The story as written assumes facts that do not hold. Three findings, all verified against the tree at `13b89763`:

### M1. `13b89763`'s outbound-dedup is **in-memory only** — there is no SQLite table with `draft_text`/`sent_text`.

`git show 13b89763 --stat` shows the dedup landed in `src/channels/imessage.c` as a fixed-size ring on the iMessage channel context. The fields are:

```c
/* src/channels/imessage.c (post-13b89763) */
char     sent_ring[HU_IMESSAGE_SENT_RING_SIZE][HU_IMESSAGE_SENT_PREFIX_LEN];
size_t   sent_ring_len[HU_IMESSAGE_SENT_RING_SIZE];
uint32_t sent_ring_hash[HU_IMESSAGE_SENT_RING_SIZE];
uint32_t sent_ring_target_hash[HU_IMESSAGE_SENT_RING_SIZE];
int64_t  sent_ring_ts[HU_IMESSAGE_SENT_RING_SIZE];
size_t   sent_ring_full_len[HU_IMESSAGE_SENT_RING_SIZE];
size_t   sent_ring_idx;
```

The ring stores **what was actually sent** so a duplicate send-attempt can be silently dropped at the channel boundary. It does **not** store the agent's draft, does not store a "before edit" / "after edit" distinction, is **not persisted across process restarts**, and lives only inside `hu_imessage_ctx_t` (one ring per channel instance). `rg "draft_text|sent_text"` returns zero hits across `src/` and `include/`.

In other words: the canonical signal the story asks for — "every outbound message I edit or reject before sending" — **does not exist as data anywhere in the repo today**. The dedup ring is a *send-side duplicate suppressor*, not a *correction recorder*.

### M2. `hu_training_data_extract_dpo` already exists and already mines `dpo_pairs`.

`include/human/ml/training_data_extractor.h:64` and `src/ml/training_data_extractor.c:378`:

```c
hu_error_t hu_training_data_extract_dpo(hu_allocator_t *alloc,
                                        const char *memory_db_path,
                                        int correction_window_sec,
                                        size_t *pairs_created);
```

It walks the `messages` table (chat history in `~/.human/memory.db`) and emits a `(prompt, chosen, rejected)` triple wherever the pattern `user(N) → assistant(N+1) → user(N+2)` occurs within `correction_window_sec` (default `HU_DPO_CORRECTION_WINDOW_SEC = 300`). The `rejected` is the agent's response; the `chosen` is the user's followup; the `prompt` is the original user message. Writes go straight to `dpo_pairs`. The miner uses a tracking table `dpo_auto_extractions(msg_id)` to avoid re-mining the same assistant turn.

**That is not the signal US-7.2 asks for.** US-7.2 wants: agent **drafts** message D, user **edits** it to E, channel **sends** E. The triple is `(prompt=incoming_context, rejected=D, chosen=E)`. The existing extractor's `rejected` is the agent's reply that was actually sent; it never sees a draft that was discarded. The two signals are distinct and complementary.

### M3. The PII function the story names does not exist.

The story says `hu_personal_model_redact_pii`. The actual function is:

```c
/* include/human/ml/training_data_quality.h:75 */
hu_error_t hu_pii_redact(const char *text, size_t text_len,
                         char *out, size_t out_cap, size_t *out_len,
                         hu_pii_stats_t *stats);
```

Already called from `src/ml/training_data_extractor.c:319,332` on conversation content before it flows into LoRA training files. This is the correct redactor and the same one US-7.2 should use. AC-7.2.4 will pass if we route prompt/chosen/rejected through `hu_pii_redact` before insert.

### Implication

AC-7.2.1 cannot be satisfied as literally written because the **data source it names** (`outbound-dedup table` with a `draft_text != sent_text` row) does not exist. AC-7.2.2 is the same problem. AC-7.2.3 (consumed by `finetune-gemma.py --from-corrections`), AC-7.2.4 (PII), AC-7.2.5 (dedup), AC-7.2.6 (clean compile) are all satisfiable under any of the options below.

We have to either (a) build the missing data source, (b) reinterpret the AC against an existing data source, or (c) BLOCK and bounce to PO.

---

## 1. Options considered

| Option | Cost | AC-7.2.1 / AC-7.2.2 satisfiable? | Carries real preference signal? |
|---|---|---|---|
| **(a)** Persist outbound drafts: add a new `outbound_drafts(channel_chat_id, draft_text, sent_text, ts)` SQLite table written by `imessage_send` (and any other outbound channel), then mine it. | **Large.** Touches channel hot path (high-risk tier — `src/channels/`), needs schema migration, needs every outbound channel to participate or the miner only sees one channel's worth of data. Risk of breaking the 7 dedup tests in `tests/test_imessage_outbound_dedup.c`. | Yes, literally. | Yes — only if there's an actual edit pipeline producing `draft_text != sent_text`. There isn't one today (no UI/CLI step where the user reviews a draft and edits it before send). So the table would persist a column that's always equal to the other column. Zero real pairs until a draft-review UI exists. |
| **(b)** New small miner module that reuses the **already-mined** user-correction signal from `messages` but tags it `source="outbound_edit"`, routes everything through `hu_pii_redact`, adds content-hash dedup (which the existing extractor lacks), and exposes it as `human ml mine-corrections`. | **Small.** ~200 LOC new file + 1 dispatcher line + 1 test file. No channel changes, no schema migration, no risk to imessage send path. | Yes, with a re-interpretation: "outbound correction" = "user's followup turn that the existing `hu_training_data_extract_dpo` already identifies as a correction." The `rejected`/`chosen` semantics match (`rejected` = agent's sent message, `chosen` = user's correcting followup). | Yes — same signal as the existing extractor, just cleanly packaged as a CLI entry point with PII redaction and content-hash dedup added on top. |
| **(c)** Mark BLOCKED, surface to PO that AC-7.2.1/.2 reference a data source that doesn't exist. | Zero implementation cost; full sprint-day cost. | N/A | N/A |

**Pick: (b).** Justification:

1. **It is the cheapest path that produces a working `human ml mine-corrections` command writing real `(prompt, chosen, rejected)` rows to `dpo_pairs`** — which is what US-7.1 (DPO pass) and US-7.5 (nightly cron) actually need to consume. The downstream stories don't care whether the correction signal came from a draft-edit ring or from a user-followup pattern; they care that `dpo_pairs` has rows.
2. **It builds infrastructure (`human ml mine-corrections` CLI, content-hash dedup, PII pipe-through, in-memory SQLite test fixture) that is reusable for option (a) later** if a draft-review UI ever exists and we need to mine its persisted drafts.
3. **Option (a) ships a feature with no producer.** Persisting `draft_text != sent_text` rows from a code path where draft always equals sent yields zero pairs and is pure infra debt.
4. **Risk tier stays MEDIUM** (story's declared tier). Option (a) escalates to HIGH (`src/channels/`).

Option (b) requires one AC re-interpretation, documented in §7. Recommend scrum-master gets a one-line PO sign-off before implementer starts; if PO insists on literal AC-7.2.1, escalate to BLOCKED and revisit (a).

---

## 2. Approach (Option (b), in one paragraph)

Add `src/ml/dpo_miner.c` + `include/human/ml/dpo_miner.h` exporting `hu_dpo_mine_corrections(hu_allocator_t*, sqlite3 *db, const hu_dpo_mine_opts_t*, size_t *pairs_recorded)`. The miner opens the `messages` table read-only, runs the same 3-turn correction SQL the existing extractor uses, redacts each of `prompt`/`chosen`/`rejected` through `hu_pii_redact`, computes a `(prompt_hash, chosen_hash, rejected_hash)` triple via FNV-1a (already used by the imessage ring), and inserts into `dpo_pairs` via `hu_dpo_record_pair` only when the triple is not already present in a new tracking table `dpo_pair_hashes(prompt_hash, chosen_hash, rejected_hash, UNIQUE)`. Source string is hardcoded to `"outbound_edit"` per AC-7.2.1. The miner is invoked by a new subcommand `human ml mine-corrections` registered in `src/main.c`. Persistence boundary: caller owns the `sqlite3*` (the CLI opens `~/.human/memory.db`, tests pass `:memory:`). The miner is read-only against `messages` and writes only to `dpo_pairs` + `dpo_pair_hashes`. JSONL export to `dpo/pairs.jsonl` for US-7.1 consumption reuses the existing `hu_dpo_export_jsonl`.

---

## 3. Concrete file plan

| Action | Path | Purpose | Est. LOC |
|---|---|---|---|
| ADD | `include/human/ml/dpo_miner.h` | Public header: `hu_dpo_mine_opts_t`, `hu_dpo_mine_corrections`, `hu_dpo_mine_stats_t`. | +60 |
| ADD | `src/ml/dpo_miner.c` | Implementation: 3-turn SQL → PII redact → hash → dedup check → `hu_dpo_record_pair`. | +280 |
| MODIFY | `src/ml/cli.c` | Add `hu_ml_cli_mine_corrections` following the `hu_ml_cli_dpo_train` pattern (lines 484-596). Opens DB, calls miner, prints stats, closes DB. `HU_IS_TEST` early-return like its peers. | +90 |
| MODIFY | `src/main.c` | Register subcommand: `if (strcmp(sub, "mine-corrections") == 0) return hu_ml_cli_mine_corrections(...)` inserted after the `dpo-train` block at line 248. Help text added to both help blocks (lines 228 and 272). | +5 |
| MODIFY | `CMakeLists.txt` | Add `src/ml/dpo_miner.c` to the `human_core` source list (mirror where `src/ml/dpo.c` is listed). | +1 |
| ADD | `tests/test_dpo_miner.c` | 6 test functions, one per AC. Uses `:memory:` SQLite. | +420 |
| MODIFY | `tests/test_main.c` | Register `extern_test_dpo_miner` suite. Follow pattern at the bottom of the file (see `test_imessage_outbound_dedup` registration added in `13b89763`). | +3 |
| READ-ONLY-DEP | `src/ml/dpo.c` | Reuse `hu_dpo_collector_create`, `hu_dpo_init_tables`, `hu_dpo_record_pair`. Schema for `dpo_pairs` is exactly as declared in `hu_dpo_init_tables` (see §4). | — |
| READ-ONLY-DEP | `src/ml/training_data_quality.c` | Call `hu_pii_redact` (signature in §4) for prompt/chosen/rejected. | — |
| READ-ONLY-DEP | `src/ml/training_data_extractor.c` | Mirror the correction-detection SQL (lines 422-441) verbatim — we want bit-identical semantics so US-7.5's nightly cron sees a coherent miner-extractor relationship. | — |
| OUT-OF-SCOPE | `src/channels/imessage.c` | Not touched. Option (a) is deferred. | — |

---

## 4. Existing-code interface notes (exact signatures)

### 4.1 PII redactor — `include/human/ml/training_data_quality.h:75`

```c
hu_error_t hu_pii_redact(const char *text, size_t text_len,
                         char *out, size_t out_cap, size_t *out_len,
                         hu_pii_stats_t *stats);
```

Behavior we rely on:
- Output is NUL-terminated when there is room.
- Truncation is silent; `out_cap` must be `text_len + 16` worst case.
- Pure scan, no allocations. Safe to call back-to-back on `prompt`, `chosen`, `rejected`.
- Patterns covered: email → `[EMAIL]`, phone → `[PHONE]`, SSN → `[SSN]`, CC-digits → `[CC]`, IPv4 → `[IP]`, anchored secrets → `[SECRET]`. Contact-name redaction (the story's example) is NOT covered by `hu_pii_redact` — emails inside contact records are, but bare first/last names are not. **Implementer must not over-claim AC-7.2.4 coverage; the test name `miner_redacts_pii` should assert email + phone + SSN, not bare names.** Flagged for PO in §7.

### 4.2 DPO recorder — `include/human/ml/dpo.h:49`

```c
hu_error_t hu_dpo_record_pair(hu_dpo_collector_t *collector,
                              const hu_preference_pair_t *pair);
```

`hu_preference_pair_t` field caps (must not be exceeded — fields are fixed-size, not pointers): `prompt[2048]`, `chosen[4096]`, `rejected[4096]`, `source[64]`. Miner must truncate redacted output to fit. Truncation policy: drop the pair entirely if the redacted prompt exceeds 2048 (training pairs above that length hurt LoRA more than dropping them — same call the existing extractor implicitly makes by inheriting these caps).

### 4.3 SQLite schema written by `hu_dpo_init_tables` (`src/ml/dpo.c:41-45`)

```sql
CREATE TABLE IF NOT EXISTS dpo_pairs(
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    prompt    TEXT,
    chosen    TEXT,
    rejected  TEXT,
    margin    REAL,
    timestamp INTEGER,
    source    TEXT
);
```

**No UNIQUE constraint.** This is the source of the dedup risk in §6. The miner must guard against double-insert at the application layer.

### 4.4 Messages table the miner reads from (`src/memory/engines/sqlite.c:1276`)

```sql
INSERT INTO messages (session_id, role, content) VALUES (?1, ?2, ?3)
```

`messages` columns the miner reads: `id`, `session_id`, `role`, `content`, `created_at`. Same columns the existing `hu_training_data_extract_dpo` reads (see SQL at `src/ml/training_data_extractor.c:423-441`). The miner reuses that SQL verbatim — copy/paste with the only diff being the source-tag.

### 4.5 Tracking tables

- Existing: `dpo_auto_extractions(msg_id, extracted_at)` written by `hu_training_data_extract_dpo`. The new miner deliberately does **not** write to this table — sharing it would conflate the two miners and make either one's `--reset` operation corrupt the other.
- New: `dpo_pair_hashes(prompt_hash INTEGER, chosen_hash INTEGER, rejected_hash INTEGER, recorded_at INTEGER, PRIMARY KEY(prompt_hash, chosen_hash, rejected_hash))`. Used for AC-7.2.5 dedup. FNV-1a 64-bit (the hash used by `imessage_hash` in the dedup ring; relocate or re-derive — single static `dpo_miner_hash64(const char*, size_t)` in `dpo_miner.c`).

### 4.6 CLI dispatcher pattern (`src/main.c:247-264`)

Each subcommand is dispatched as:

```c
if (strcmp(sub, "<name>") == 0)
    return hu_ml_cli_<name>(alloc, argc - 2, (const char **)(argv + 2));
```

`hu_ml_cli_<name>` signatures live in `src/ml/cli.c` and follow the shape of `hu_ml_cli_dpo_train` at line 484. The `HU_IS_TEST` early-return pattern at line 523 is mandatory — `human ml mine-corrections` must not open a real SQLite file when `HU_IS_TEST` is defined.

---

## 5. Test plan

Test file: `tests/test_dpo_miner.c`. Fixture builder: a single helper `dpo_miner_open_fixture_db(sqlite3 **out, const fixture_msg_t *rows, size_t n)` that opens `:memory:`, creates the `messages` table with the same schema as the SQLite memory engine, inserts deterministic rows with explicit `created_at` timestamps (no `CURRENT_TIMESTAMP` — non-deterministic), and returns the db. Cleanup: `sqlite3_close` in each test.

| AC | Test function | What it asserts | Fixture |
|---|---|---|---|
| 7.2.1 | `miner_records_outbound_edit_pair_with_correct_fields` | After `hu_dpo_mine_corrections` runs over a fixture with one valid correction triple, `SELECT prompt, chosen, rejected, margin, source FROM dpo_pairs` returns exactly one row with `source = "outbound_edit"`, `margin = 0.5`, and the three text fields matching the fixture (post-redaction). | 3 rows: user / assistant / user, all timestamps within 60s, no PII. |
| 7.2.2 | `miner_skips_unedited_messages` | After running over a fixture where every user followup is >300s after the assistant turn (no correction signal), `SELECT COUNT(*) FROM dpo_pairs` returns 0. (Note: this is the closest faithful test for "no edit, no pair" given the re-interpretation in §7 — there is no "draft == sent" case to skip because we are not on the draft path.) | 3 rows: user / assistant / user, gap = 600s. |
| 7.2.3 | `miner_writes_db_at_path_resolvable_by_finetune_script` | After miner runs, `hu_dpo_export_jsonl` against the same collector writes a parseable JSONL to a temp path; first line is a valid JSON object with keys `prompt`, `chosen`, `rejected`. (Faithful proxy for the Python-side test — the script-side test lives in `tests/test_finetune_gemma_dpo.py` per the AC.) | Same as 7.2.1. |
| 7.2.4 | `miner_redacts_pii` | Fixture has an email and a phone number in the assistant's response and in the user's followup. After miner runs, `SELECT chosen, rejected FROM dpo_pairs` returns rows containing `[EMAIL]` and `[PHONE]` and **not** containing the original PII substrings. Asserts via `strstr`. (Contact-name redaction is documented as out of scope per §4.1 / §7.) | 3 rows with `seth@example.com` and `+1-555-123-4567` in assistant + user-followup. |
| 7.2.5 | `miner_deduplicates_pairs` | Run miner twice over the same fixture. First call: `*pairs_recorded == 1`. Second call: `*pairs_recorded == 0`. `SELECT COUNT(*) FROM dpo_pairs` is `1` both times. Also verifies a *different* fixture inserted between the two runs gets recorded on the second pass (i.e., the dedup is content-keyed, not run-keyed). | Two 3-message fixtures with identical text, then a third with different text. |
| 7.2.6 | (no test function — gate is the build itself) | Verified by running `cmake --preset dev && cmake --build --preset dev && ./build/human_tests --suite=DpoMiner` under CI. Preset `dev` enables ASan + `-Wall -Wextra -Wpedantic -Werror`. CI workflow `.github/workflows/ci.yml` already runs `human_tests` in full; the new test file is picked up automatically once `test_main.c` registers it. | — |

Tests register through `tests/test_main.c` — same registration pattern used by `test_imessage_outbound_dedup` (added in `13b89763`).

**Test isolation guarantees:**
- No `~/.human/` writes. All DBs are `:memory:`.
- No real network (miner does no network anyway).
- `time(NULL)` is fine for `timestamp` in `hu_preference_pair_t` because no test asserts on the value; tests assert on text fields and counts only.
- Each test calls `sqlite3_close` before returning.

---

## 6. Risk analysis

### R1. AC-7.2.1 literal interpretation is unsatisfiable (HIGH probability / MEDIUM impact)

**What could go wrong:** PO reads the literal AC ("outbound-dedup table contains a row where draft_text != sent_text"), the test we wrote (3-turn correction pattern) does not match, and the story bounces back at review.

**Mitigation:** Scrum-master gets explicit PO sign-off on §7 reinterpretation *before* the implementer starts. Two minutes of conversation now saves a rebuild later. If PO insists on literal AC-7.2.1, this story becomes BLOCKED and option (a) gets a separate design pass with HIGH risk-tier review.

### R2. PII leak — bare contact names pass through (MEDIUM / MEDIUM)

**What could go wrong:** A user message like "Mindy said she's coming over" contains a contact name that `hu_pii_redact` does NOT redact (the redactor handles emails/phones/SSN/CC/IP/secrets, not free-form proper nouns). That name lands in `dpo_pairs.prompt`, then in `dpo/pairs.jsonl`, then in LoRA training data, then memorized by the adapter weights. Adapter weights are local-only, but the `dpo/pairs.jsonl` file is the same file shape consumed by US-7.1's `mlx_lm lora --fine-tune-type dpo` — a future user who shares an adapter for support has leaked Mindy's first name.

**Mitigation:**
- Document explicitly in the miner's header that `hu_pii_redact` covers structured PII only.
- AC-7.2.4 test only asserts on `[EMAIL]` / `[PHONE]` / `[SSN]`, not names.
- Flag to PO in §7 as a known coverage gap; surface as a follow-up story candidate (named-entity redaction is a real piece of work — needs an NER model or contact-list lookup against the personal model, not a regex).
- Out-of-scope for US-7.2 per the story's own "Out of scope" line.

### R3. Concurrent miner + agent writer race on `dpo_pairs` (MEDIUM / SMALL)

**What could go wrong:** `dpo_pairs` has no UNIQUE constraint (§4.3). If the W14 nightly cron runs `human ml mine-corrections` while a user's foreground turn lands a `hu_dpo_record_from_feedback` or `hu_dpo_record_from_retry` call (both at `src/ml/dpo.c:107,141`) on the same DB, two near-simultaneous inserts can land. Same row hash, two rows. The miner's `dpo_pair_hashes` UNIQUE PK protects against the **miner inserting twice**, but it does not protect against the **miner racing the agent** because the agent paths don't write to `dpo_pair_hashes`.

**Mitigation:**
- Accept it for this story. The agent's feedback/retry paths use distinct `source` values (`"user_feedback"` and `"reflection_retry"`) — a downstream DPO trainer that gets both rows trains on both signals, which is *more* signal, not corruption.
- A clean fix is to add a UNIQUE constraint on `(source, prompt, chosen, rejected)` at the table level, but that's a schema migration touching `hu_dpo_init_tables` and would affect every existing caller (currently no on-disk migration framework exists for `dpo_pairs` — its `CREATE TABLE IF NOT EXISTS` would silently no-op against an existing prod DB without the new constraint). That's option (a)'s problem; scope it to a follow-up.
- Note this in the miner's header comment so the next person doesn't trip on it.

---

## 7. Open questions / required PO sign-off

**Q1 (BLOCKING for clean AC pass).** AC-7.2.1 says "outbound-dedup table contains a row where `draft_text != sent_text`." That table does not exist (M1). The cheapest path forward — option (b) — reinterprets the AC as "the user-correction signal already mined by `hu_training_data_extract_dpo`, repackaged as a `human ml mine-corrections` CLI with PII redaction and content-hash dedup added, tagged `source = "outbound_edit"`." This satisfies the *intent* (DPO pairs from real editorial decisions, no manual labels) but not the *letter* (no draft-vs-sent column). **Need PO yes/no before implementer starts.**

**Q2.** AC-7.2.4 names "contact name or email address" as the PII pattern. `hu_pii_redact` covers email but not contact names. Three options:
- (a) Drop the "contact name" half from AC-7.2.4, ship email/phone/SSN coverage, file follow-up for NER.
- (b) Add a contact-list lookup against `hu_personal_model_t.identity_links` (loaded by `13b89763`'s identity-links parser) inside the miner. Adds ~80 LOC and a dependency on the personal-model artifact existing at miner-run time.
- (c) Block on this until an NER-grade redactor exists (separate story).
Recommend (a). PO call.

**Q3.** AC-7.2.3 expects `scripts/finetune-gemma.py --dpo --from-corrections` to "locate the output `dpo_pairs.db` in its standard candidate search path." Today the miner writes to whatever DB path the CLI was pointed at — typically `~/.human/memory.db`. The Python script's candidate search path lives in `scripts/finetune-gemma.py` (not read here). Either:
- (a) The miner additionally exports a JSONL to a known location (e.g. `~/.human/dpo/pairs.jsonl`), and US-7.1 reads from that. Clean separation.
- (b) The Python script learns to open `memory.db` directly and `SELECT FROM dpo_pairs`. Adds SQLite dependency on the Python side.
Recommend (a). Confirm with the US-7.1 owner so both stories land coherent.

---

## 8. Sequencing (for the implementer)

1. **Skeleton: header + empty impl.** Create `include/human/ml/dpo_miner.h` and `src/ml/dpo_miner.c` with the `hu_dpo_mine_corrections` signature and a body that returns `HU_OK` with `*pairs_recorded = 0`. Add to `CMakeLists.txt`.
   - Verify: `cmake --build --preset dev` → builds clean.

2. **Test skeleton: register a failing test.** Add `tests/test_dpo_miner.c` with one stub test `miner_records_outbound_edit_pair_with_correct_fields` that asserts `pairs_recorded == 1` (will fail). Register suite in `tests/test_main.c`.
   - Verify: `./build/human_tests --suite=DpoMiner` → fails as expected.

3. **Fixture helper.** Implement `dpo_miner_open_fixture_db(...)` in `tests/test_dpo_miner.c`. Verify it can insert and read back 3 rows from an in-memory DB.
   - Verify: temp test that inserts and re-selects from `messages` → passes.

4. **Core miner logic.** Implement the 3-turn correction SELECT in `dpo_miner.c` (copy SQL verbatim from `src/ml/training_data_extractor.c:423-441`, change source-tag). No PII, no dedup yet. Build the `hu_preference_pair_t`, call `hu_dpo_record_pair`. Margin = 0.5, source = `"outbound_edit"`.
   - Verify: `./build/human_tests --suite=DpoMiner --filter=miner_records_outbound_edit_pair_with_correct_fields` → passes.

5. **PII redaction pass.** Wrap each of `prompt`, `chosen`, `rejected` through `hu_pii_redact` before assigning to the pair struct. Add `miner_redacts_pii` test.
   - Verify: `./build/human_tests --suite=DpoMiner` → both tests pass.

6. **Content-hash dedup.** Add `dpo_pair_hashes` table creation, FNV-1a hashing of all three redacted fields, `INSERT OR IGNORE` semantics. Add `miner_deduplicates_pairs` and `miner_skips_unedited_messages` tests.
   - Verify: `./build/human_tests --suite=DpoMiner` → 4 tests pass.

7. **CLI wiring.** Add `hu_ml_cli_mine_corrections` in `src/ml/cli.c` (mirror `hu_ml_cli_dpo_train`). Add dispatch + help text in `src/main.c`. `HU_IS_TEST` early-return.
   - Verify: `./build/human ml mine-corrections --help` → prints usage. `./build/human_tests` full suite → 10280+ tests pass, zero ASan errors. (AC-7.2.6.)

8. **JSONL export entry point.** Add an optional `--export-jsonl <path>` flag to the CLI that, when set, calls `hu_dpo_export_jsonl` after mining. Add `miner_writes_db_at_path_resolvable_by_finetune_script` test.
   - Verify: `./build/human_tests --suite=DpoMiner` → 5 tests pass. Then `cmake --preset dev && cmake --build --preset dev && ./build/human_tests` → full suite green, ASan clean.

After step 8, run `/verify` per the project's quality-gate protocol. Hand back to scrum-master for critic review.

---

## 9. Acceptance criteria mapping

| AC | Coverage | Notes |
|---|---|---|
| 7.2.1 | `miner_records_outbound_edit_pair_with_correct_fields` | **Conditional on §7 Q1 PO sign-off.** Tagged `source = "outbound_edit"`, margin = 0.5. |
| 7.2.2 | `miner_skips_unedited_messages` | Reinterpreted as "no correction signal in window ⇒ no pair." |
| 7.2.3 | `miner_writes_db_at_path_resolvable_by_finetune_script` + Python-side test in US-7.1 | Coordinate with US-7.1 owner per §7 Q3. |
| 7.2.4 | `miner_redacts_pii` | Email + phone + SSN. Contact-name redaction is §7 Q2 — recommend dropping from this story. |
| 7.2.5 | `miner_deduplicates_pairs` | New `dpo_pair_hashes` table with PK on `(prompt_hash, chosen_hash, rejected_hash)`. |
| 7.2.6 | Build + full-suite run under `cmake --preset dev` | Enforced by CI workflow `ci.yml`. |
