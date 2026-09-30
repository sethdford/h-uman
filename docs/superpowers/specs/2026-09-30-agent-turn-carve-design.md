---
title: Carve hu_agent_turn into turn stages — phase 1
status: draft
date: 2026-09-30
---

# Carve `hu_agent_turn` into turn stages — phase 1

## 1. Problem (measured on origin/main 850183481)

`hu_agent_turn` (`src/agent/agent_turn.c:1525-10420`) is one function of
**8,896 lines** that runs on every reply. It is the largest single liability in
the DDD program (`docs/plans/2026-05-29-ddd-bounded-contexts/README.md`), and
E4 has no plan for it.

| Counter | Value | Ceiling / target |
|---|---|---|
| `hu_agent_turn` length | 8,896 | `MAX_FN_BASELINE` 8,943 / target 300 |
| `src/agent/agent_turn.c` | 10,420 | file ceiling 10,420 (at it) / target 800 |
| `src/daemon.c` | 10,420 | at the same ceiling |
| flat `src/agent/*.c` | 162 | no ratchet; grew from 157 |

Consequences today: every feature on the reply path must fit inside this
function, and the file has **zero** lines of headroom. The October retrieval
work (lexical+dense fusion) lands in its retrieval stage.

Structure facts that shape the design (recon 2026-09-30):

- **No cleanup label.** 24 `return`s, each freeing a hand-picked subset of
  ~60 owned `char*`/`len` pairs. Some exits leak (3137 misses the frontier
  contexts and `acp_context`; 4138 misses skills/persona/awareness; the
  `HU_ERR_TIMEOUT` exit at 10416 never frees `plan_ctx`).
- **A freed-and-NULLed pointer is load-bearing.** `memory_ctx` is freed at
  ~4623 and was read after free at ~8007 (M4 fix NULLs it). Copying the
  pointer into a struct without the same NULLing reintroduces the bug.
- **Streaming duplicate.** `hu_agent_turn_stream_v2` (`agent_stream.c:295-3023`)
  is the daemon's primary inbound route; 149 of its 166 distinct `hu_*`
  callees also appear in `hu_agent_turn`. Its retrieval stage diverges (fixed
  10/4000 budget, no Self-RAG, no W12 merge).
- **No output characterization exists.** 16 test files call `hu_agent_turn`
  82 times; the only request capture (`test_e2e.c:116`) keeps the first system
  message in a 16 KB buffer against a 24 KB prompt budget.

## 2. Goal and success criteria

Move whole stages out of `hu_agent_turn` into `src/agent/turn/` with small
interfaces, **without changing behaviour**, one stage per PR, with every gain
locked by a ratchet.

Phase 1 (this spec) is done when:

1. A recording-provider **characterization harness** pins the complete provider
   requests and final response of a corpus of turns (§4.1), and passes on the
   unmodified code.
2. Stages **S3, S2, S0, S8, S4, S16** (§3) are extracted, each proven
   byte-identical by that harness.
3. `hu_agent_turn` ≤ **5,900** lines and `agent_turn.c` ≤ **7,500** lines,
   both locked by the existing ratchets.
4. A new ratchet caps flat `src/agent/*.c` at its current count (§4.5).
5. Full suite green, ASan clean, every ratchet at or below its baseline.

Out of scope for phase 1: S5/S6 prompt assembly, the S9–S12 loop body, S13–S15
learning/guards/persist (they hold ~55 of the 63 raw SQLite sites and the
highest ownership risk), unifying `stream_v2` onto the extracted stages, and
fixing the leaks listed in §1 (tracked separately, §5).

## 3. Stage map and phase-1 order

Line ranges are origin/main 850183481 and will drift; each stage PR
re-measures before cutting (E2 lesson: stale line numbers misled v1).

| # | Stage | Lines | Returns | Raw SQLite | Phase |
|---|---|---|---|---|---|
| S0 | entry, caches, slash commands, input guard | 1525–1657 | 7 | 0 | 1 |
| S1 | plan + ingest | 1659–1827 | 1 | 0 | later |
| S2 | perception (cognition budget, STM, radar, tone, prefs) | 1829–2180 | 0 | 0 | 1 |
| **S3** | **retrieval** (Self-RAG, memory loader, graph grounding, W12 merge) | 2182–2405 | 0 | 0 | **1, first** |
| S4 | context builders (12 owned `*_ctx`) | 2407–3099 | 0 | 3 | 1 |
| S5 | persona + humanness + frontiers | 3101–4076 | 1 | 8 | later |
| S6 | prompt assembly | 4078–5069 | 2 | several | later |
| S7 | loop setup | 5071–5227 | 0 | 0 | later |
| S8 | silence gate | 5229–5334 | 1 | 1 | 1 |
| S9–S12 | iteration prep, provider, HuLa, final quality | 5336–7662 | 7 | many | later |
| S13–S15 | learning, outbound guards, persist | 7663–8718 | 1 | ~25 | later |
| S16 | tool dispatch | 8720–10246 | 1 | 0 | 1 |
| S17–S18 | iteration tail, exhausted | 10247–10420 | 1 | 0 | later |

Order and why:

1. **S3 retrieval → `turn/turn_retrieve.c`.** No returns, no raw SQLite, one
   `#ifdef`; it is the October landing zone. Preserve the W12 merge's
   freeing of `graph_ctx` (2393–2398) exactly; document it for the retrieval
   work, which owns changing it.
2. **S2 perception → `turn/turn_perceive.c`.** No returns, no SQLite.
3. **S0 entry → `turn/turn_entry.c`.** Seven early returns become one
   `hu_turn_step_t` result (`CONTINUE` / `RETURN(err)`), per the d8aca3a04
   precedent for early `continue`/`break`.
4. **S8 silence → `turn/turn_silence.c`.** Its return frees 14 items, so it
   needs `hu_turn_ctx_free` (§4.2) first.
5. **S4 context builders → `turn/turn_context.c`.** Three raw SQLite sites move
   behind existing or new repository calls (`src/memory/repos/`), or the stage
   stays out of phase 1 if a site has no repository yet.
6. **S16 tool dispatch → `turn/turn_tools.c`.** 1,527 lines; includes the
   LLMCompiler DAG (`pthread_create` with a stack `works[]` array at ~8865 —
   the ASan pthread-aliasing shape; move it to the heap per
   `.claude/rules/asan-pthread-stack-aliasing-darwin.md` in a separate PR
   before the move). Its cloned `ca_msg`/`hs_msg` blocks (9611/9644 vs
   10072/10105) are factored into one helper in the same PR, lowering the
   clone ratchet.

## 4. Design

### 4.1 Characterization harness (built first, PR 0)

- `tests/turn_recording_provider.{c,h}`: a shared mock `hu_provider_t` that
  **deep-copies** every `hu_chat_request_t` (all messages with roles, tools,
  model, temperature, max_tokens) — messages live in the per-iteration
  `turn_arena`, so pointers must not be kept — and replays scripted responses,
  including tool calls.
- `tests/test_agent_turn_characterization.c`: a corpus of ≥ 24 scripted turns
  covering each phase-1 stage's branches — slash command, input-guard reject,
  cache hit, Self-RAG skip/verify, graph grounding on/off, silence path, one
  and two tool iterations, DAG dispatch, provider error, iteration exhaustion.
  For each turn it records the full request sequence and the final
  `response_out`/error, serializes them deterministically, and compares with a
  golden fixture under `tests/fixtures/agent_turn_golden/`.
- Determinism: `hu_time_set_test_override_ms` pins the clock; the harness
  unsets or pins the 15 environment reads the function consults
  (`HU_DEBUG`, `HU_SELF_RAG_MODE`, `HU_SALIENCE*`, `HU_CONTINUITY_CTX`, …).
  The 35 raw `time(NULL)` calls that bypass the override are listed in the
  harness; any that reach request bytes are routed through the time helper
  in PR 0 (a mechanical change the harness itself proves harmless).
- Goldens are generated once from **unmodified** code and committed; a stage
  PR may not regenerate them. The harness runs in the normal suite and in
  ASan.
- Limitation, stated in the harness header: it runs under `HU_IS_TEST`, so the
  planner, ToT, native HuLa, constitutional, LLMCompiler and HuLa-IR blocks
  (all `#ifndef HU_IS_TEST`) are not characterized. Phase-1 stages that
  contain such blocks (S16's HuLa IR) are moved **verbatim with their guards**
  and proven by review plus the source-presence test in §4.4.

### 4.2 The per-turn context

- `include/human/agent/turn.h` defines `hu_turn_ctx_t`, heap-allocated per turn
  (never on the stack — the turn runs on a worker thread; see the ASan pthread
  rule), grouped as: `in` (agent, msg, len, out pointers), `perception`,
  `retrieval`, `prompt_parts`, `loop`, `post`.
- It grows **only as stages move**: each stage PR adds exactly the fields its
  measured surface needs (E2 precedent cc1d0fe9f: build the struct from the
  block's measured inputs and outputs, not from a design guess).
- `hu_turn_ctx_free(ctx)` frees every owned field and NULLs it. Adoption is
  incremental: a stage writes into the struct, and `hu_agent_turn` unpacks the
  results back into its existing locals so the ~700 downstream uses stay
  untouched until their own stage moves (d8aca3a04 precedent). The unpack is
  a move of ownership: the struct field is NULLed so nothing is freed twice.
- `memory_ctx` rule: whichever of the struct or the local owns it is NULLed at
  the point the original code frees it (~4623). A test in the harness corpus
  reaches the post-4623 read path.

### 4.3 Stage interface shape

Each stage is `hu_error_t hu_turn_<stage>(hu_turn_ctx_t *ctx)` (S0 returns a
`hu_turn_step_t`), lives in `src/agent/turn/turn_<stage>.c`, and has its own
contract test file `tests/test_turn_<stage>.c` exercising the stage directly
with a hand-built context — beyond the end-to-end characterization.

Rules for every stage PR:

- **Move, don't rewrite.** Code moves verbatim; only local-to-field renames and
  the return-to-step translation are allowed. `#ifdef` stacks are mirrored; a
  guard spanning a cut is closed before it and reopened after (d8aca3a04).
- **Behaviour identical.** Characterization goldens unchanged; full suite
  green; the moved block's log lines byte-identical.
- **Preserve existing leaks and quirks.** A stage PR does not fix a leak or
  the W12 `graph_ctx` free; those are separate `fix(...)` PRs with their own
  tests (one concern per change).
- **Function statics move with their block** (`otlp_trace`/`otlp_inited`,
  `agent_turn_debounce` stay where their block is in phase 1). Thread-local
  `current agent` handling is untouched.
- **No new raw SQLite includers** under `src/agent/turn/` (sqlite-includer
  ratchet, baseline 87). No `providers/factory.h`, no channel-name `memcmp`
  (agent-core boundary).
- Ratchets lower in the same commit via auto-lock (function length, file
  size, clone).

### 4.4 Guard for untested blocks

`tests/test_turn_sources.c` (source-presence style, like
`test_daemon_feeds_the_catcher_inbound_text_only`) pins, per moved stage, that
each `#ifndef HU_IS_TEST` block from the original is present exactly once in
its new file with the same guard. It reads paths relative to the repo root
and skips with a message when not run from the root.

### 4.5 New ratchet: flat `src/agent/*.c`

`scripts/check-agent-flat-files.sh` counts `src/agent/*.c` (not
subdirectories), baseline = current count (162), fails only on growth, wired
into `.githooks/pre-commit` for `src/agent/` changes and added to
`scripts/ratchet-config.tsv` (rate auto, floor none). Each stage file lands
under `src/agent/turn/`, so the counter never grows from this program.

## 5. Known defects found by the recon (not fixed by carve PRs)

Filed as separate follow-ups, each with a failing test first:

1. Error-exit leaks at ~3137, ~4138 and the `HU_ERR_TIMEOUT` exit (`plan_ctx`).
2. S14 outbound guards rewrite `*response_out` after S13 learning has already
   recorded the pre-guard text (learning trains on text that was never sent).
3. S12 clears the thread-local current agent at ~7664 before S13 learning
   runs; nested `spawn.c` turns clobber it (cleared, not restored).
4. The W12 merge frees `graph_ctx` despite the "protected core" comment at
   ~2249 (owned by the October retrieval work).

Items 2 and 3 change behaviour and follow the OFF→SHADOW→LIVE gate rule.

## 6. Testing

- PR 0: harness + corpus + goldens from unmodified code; mutation check that
  the harness fails on (a) one changed prompt byte, (b) a reordered request,
  (c) a changed final response, (d) a dropped tool call.
- Each stage PR: goldens unchanged; stage contract tests; full suite and ASan;
  source-presence test; ratchet lines in the PR body (function length, file
  size, clone, dead-strip, sqlite includers, agent-flat-files).
- Verification per PR by the controller running the build and suite from the
  worktree root, not by the implementer's report.

## 7. Rollout and risk

Stage PRs are behaviour-identical, so no feature gate and no blind A/B. Each
merges and deploys like any refactor (`scripts/install-human-daemon.sh`,
`scripts/verify-deploy.sh`). The daemon's primary path is `stream_v2`, which
still calls `hu_agent_turn` only as a fallback — so phase-1 moves change the
code path of fallback turns first; unifying `stream_v2` onto the stages is a
later phase with its own spec, because its retrieval stage diverges and
unifying it changes behaviour.

Estimated size: PR 0 ~800 lines (harness + corpus); stage PRs 150–1,600 lines
moved each; about 8 PRs for phase 1.
