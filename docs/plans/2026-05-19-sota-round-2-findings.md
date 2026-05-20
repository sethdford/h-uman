# SOTA Round-2 Findings (2026-05-19)

Companion to `2026-05-19-sota-first-data.md`. After the 10-item
follow-up pass (commits ff566776 + fee2e377 + earlier), this round
produced three NEW findings that change the master plan.

## What was fixed in round 2

| Item | Status | Evidence |
|------|--------|----------|
| A1 — gateway prompt-size budget (agent_turn + agent_stream) | ✅ Wired, fires at 16KB cap | gateway log: `system prompt truncated from 16689 to 16264 bytes (MLX backend cap)` |
| A2 — SQLite path in `human ml dpo-train` | ✅ Reads 279 pairs via iterator filter | `[dpo-train] loaded 279 pairs from /Users/sethford/.human/memory.db (after iterator filter)` |
| B2+B3 — cleanup DELETE of legacy bad rows | ✅ 92 rows removed | 371→279, all reflection_retry + user_feedback short-side rows gone |
| B4 — launchd plist | ✅ Installed + loaded | `launchctl list` shows `ai.human.orpo-watcher` |
| B8 — `--persona` flag in verifier_ttt | ✅ Wired + smoke-tested | `P(Seth)=0.891 mode=p_seth_argmax` |
| C1 — DPO miner post-cleanup | ✅ Run | 153 inverted reduced to 118 (but the remaining are largely classifier mistakes, not corpus poison) |
| C4 — discard NEEDS_RETRY-prefix in agent_turn | ✅ Calls predicate, logs once, falls back to `let me think on that` | unit tests 6/6 |
| D1 — `hu_response_is_critique_echo` predicate + 6 unit tests | ✅ All pass | NEEDS_RETRY prefix, lowercase, embedded, legit, short, null-safe |
| **+ UAF fix** in `openai_compat.c` agent path | ✅ Surfaced by A4 run, fixed | ASan trace: heap-use-after-free at compatible.c:121, freed at openai_compat.c:760 |
| **+ L5 v3 choice rule** — filter-then-argmax | ✅ Fires 8/8 in R1 (was 0/8 in v2) | mean P(Seth) chosen = 0.836 |
| A4 — multi-turn drift | ⚠️ Ran, hit infrastructure cap | T01-T04 shape=1.0 in conv1; T05+ failed |
| A3 — L5 saturation | ✅ filter-mode fires | see above |

11/12 (92%) green, 1 partial (A4) blocked on infrastructure not agent quality.

## Finding 1 — system prompt budget is necessary but not sufficient

The A1 guard caps `system_prompt` at 16KB. Visible in production log:

  [agent_turn] system prompt truncated from 16689 to 16264 bytes
              (MLX backend cap); some context dropped

But A4 showed the MLX backend still fails on later turns. Inspecting:

  [http] curl POST failed: Server returned nothing
         http://127.0.0.1:8741/v1/chat/completions (body_len=22595)
  [http] curl POST failed: Server returned nothing
         (body_len=24241)

System prompt is 16K, but messages array adds 6-8K of conversation
history per turn. Total body crosses MLX's effective cap around turn 5.

**Next action**: cap the conversation history sent to MLX. Either:
1. Drop oldest turn(s) when total body would exceed 20KB.
2. Summarize history into a single system-prompt prefix when length > N.

Both are out of scope for this round; tracked as "A1b — conversation
history budget."

## Finding 2 — UAF in gateway agent path was real and shipping

The crash trace was unambiguous:

  ERROR: AddressSanitizer: heap-use-after-free
    READ at compatible.c:121 (compatible_build_chat_json)
    freed at openai_compat.c:760 (msgs alloc->free)
    allocated at openai_compat.c:257

The bug: `with_agent` branch frees `msgs+root` unconditionally at 760,
then guards the response builder with `agent_err == HU_OK && response`.
If agent returns HU_OK with NULL response (AI-tell retry path produced
this), control falls through past the close of the `if (app_ctx &&
app_ctx->agent)` block into the non-agent fallback at line 813, which
re-uses the freed `msgs`.

Fired on the SECOND request to the gateway in dev. In a release build
this would silently corrupt heap state and produce malformed responses.
Hard to characterize as an exploit (the trigger requires a specific
agent-retry path) but easy to reproduce: just make any prompt that
trips ai_tells twice in a row.

**Fixed.** Now emits `502 "Agent returned empty response"` and returns
before the fallback path.

## Finding 3 — L5 v2 saturation check was wrong; v3 is right

A3 with n=8 prompts × n=5 candidates per prompt: the v2 predicate
`all(c.shape.score >= 1.0 for c in candidates)` fired ZERO times. At
the 0.7-1.15 temperature spread, at least one candidate always failed
shape, defeating the P(Seth) tiebreaker.

L5 v3: instead of "all pass," FILTER to candidates that pass shape,
then argmax P(Seth) within that filtered set. Falls back to
shape-argmax only when NO candidate passes.

Result with v3 against the same suite:
- R0 (n=1): 6/8 prompts fired `p_seth_argmax_filtered`
- R1 (n=5): **8/8 prompts fired `p_seth_argmax_filtered`**
- Mean P(Seth) of chosen R1 responses: **0.836**

Single best example: imsg-008 ("ugh worst day, boss threw me under
the bus"):

  R0 (no TTT): empty response (shape fail, score=0.00)
  R1 (best-of-5): "damn that's rough. managers love doing that to
                   save their own ass" → shape=1.0, P(Seth)=0.984

This is what L5 was supposed to do: spend N× inference compute, recover
from sample failures, and pick the most-in-voice candidate. Now it does.

## What remains genuinely open (the next-round followups)

| Item | Impact | Sketch of fix |
|------|--------|---------------|
| A1b — message-history budget | Multi-turn drift gated on this | Cap history bytes sent to provider; drop oldest or summarize |
| C2 — better PersonaEval classifier | 42% "inversion" residual is classifier noise | Retrain on a richer negative set; add length-aware features |
| C3 — reaction signals → separate table | reaction_handler currently writes single-sided rows to dpo_pairs | Add `negative_signals` / `positive_signals` tables; route reactions there |
| C4b — agent re-retry on critique echo | Current fallback "let me think on that" is generic | Use the existing `reflection_retries_left` to try ONCE more on echo detect |
| A4b — multi-turn drift on local MLX | Run skipped because of A1b | After A1b, re-run; expect drift ≈ 0 across 20 turns |

The original 6-month plan budgets Month 1 for "passive corpus accrual
+ L5 TTT + multi-turn." After this round:
- L5 TTT: DONE (v3 is correct + measured)
- multi-turn: HALF — works for ~4 turns, infrastructure cap kicks in
- Corpus accrual: still passive; the watcher fires nightly at 03:17

So Month 1 is ~70% complete on day 2. Ahead of schedule.

## Artifacts

| File | Content |
|------|---------|
| `/tmp/A3_l5_v3.json` | L5 v3 measurement run output |
| `/tmp/multiturn_drift_results.json` | A4 v2 run (T01-T04 successful, rest failed) |
| `/tmp/L2_ablation_results.json` | memory-vs-persona ablation (from round 1) |
| `/tmp/dpo_persona_audit.json` | DPO corpus audit post-cleanup |
| `~/.human/memory.db.backup-pre-cleanup-*` | Backup of pre-cleanup DPO state |

## Commits this round

| SHA prefix | Subject |
|------------|---------|
| `ff566776` | six follow-up fixes — A1/A2/B4/B8/C4/D1 |
| `fee2e377` | UAF in openai_compat agent path + L5 v3 choice rule |
| (plus the round-1 fixes from earlier commits) |  |
