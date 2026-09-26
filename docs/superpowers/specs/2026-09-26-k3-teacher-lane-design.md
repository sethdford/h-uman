---
title: K3 teacher lane — offline judge ranking the student's own samples
date: 2026-09-26
status: draft (awaiting review; spike measured — drive rule FAIL, engine prefill is the blocker)
---

# K3 Teacher Lane — Design

**Date:** 2026-09-26 · **Status:** DRAFT, awaiting review · **Throughput:** measured, FAILS the drive rule (§9)

Kimi K3 (2.78T MoE) runs locally, overnight, as a **judge** that ranks the serving
student's own reply samples. Its rankings become on-policy preference pairs for the
seth-glm-air LoRA. Only after it earns trust through a pre-registered calibration gate
anchored to Seth's real replies. K3 never serves a live reply and is never fine-tuned.

---

## 1. Goal, non-goals, success

**Goal.** Close the remaining gap between the serving model and Seth. Measured
2026-09-22 at matched length, the daemon reaches **45% of Seth's specificity**
(delta −0.44, CI [−0.623, −0.263]; insider 49%, proper 30%, concrete 60%). Voice
already passes: the n=40 human gate has detection 0.225, CI [0.123, 0.350].

**Non-goals.**
- K3 does not serve live replies. At 5.6–26.5 s/token (engine README, measured on
  a 124-core Linux box), one reply takes minutes.
- K3 is not fine-tuned. The engine is inference-only: no backward pass, and the
  4-bit experts are never dequantized.
- The engine is not merged into h-uman's C tree.
- No message data leaves the machine.

**Success** (in order; each gates the next):
1. The premise check passes (§7.1).
2. K3 passes the calibration gate (§7.2).
3. A candidate adapter trained on teacher pairs passes the existing promotion chain
   plus §7.4. It must beat v5 on the human blind A/B specificity axis without
   detection rising above 0.225 by more than the CI.

A failure at any step is a valid, reportable outcome. Stop there.

## 2. Decisions (with who made them)

| # | Decision | Made by |
|---|---|---|
| D1 | K3's role is an **offline teacher**, not live serving and not a hosted API | Seth, 2026-09-26 |
| D2 | Runs **on this Mac, weights on an external NVMe**; data stays local | Seth |
| D3 | K3 **ranks GLM's own samples**; it does not rewrite them or write from scratch | Seth |
| D4 | Architecture A: K3 as a judge backend in the existing blind-A/B chain, plus a calibration gate | Seth |
| D5 | K3 outputs a **full ranking** (not best/worst) plus a verbalized confidence | design review §4 |
| D6 | Rubric target: **"most likely what Seth actually sent"**, not "most specific" | design review §4 |
| D7 | Measure prefill before buying the drive (`--layers 10` spike) | Seth |

## 3. Architecture

```
            ┌──────────── Phase A (20:00, :8741 up) ─────────────┐
chat.db ──► │ select turns ─► reply-prompt ─► memory ground ─►    │
memory.db   │ GLM × 3 samples (X-HU-Priority: batch)              │──► candidates.jsonl
            └─────────────────────────────────────────────────────┘
            ┌──── Phase B (22:30–01:45, 05:15–08:00; never 02–05) ────┐
candidates ►│ shuffle(3 GLM + hidden real) ─► :8745 k3_server ─► rank │──► rankings.jsonl
            └──────────────────────────────────────────────────────────┘
rankings ──► calibrate.py ─► docs/evaluation/k3_judge_calibration.json
                                 │ PASS & fresh
rankings ──► pair emission ─► judge_to_dpo (family kimi-k3) ─► build_v6_preference_corpus
          ─► train-glm-adapter.sh (DPO) ─► existing promotion chain + §7.4
```

| # | Component | Job | Depends on |
|---|---|---|---|
| C1 | `third_party/kimi-k3-in-c` (submodule pinned to a SHA of sethdford's fork) + `scripts/k3/build.sh` | Builds `bin/k3` into `~/.human/k3/bin/`. Weights come from `HU_K3_MODEL_DIR` / `HU_K3_TRUNK_DIR` | the fork's Makefile, Homebrew `libomp` |
| C2 | `scripts/k3/k3_server.py` | OpenAI-compatible `/v1/chat/completions` on **127.0.0.1:8745** (refuses non-loopback). Single-flight, returns only the response body, **never passes `--history`**. Owns the preemption loop and memory guard (§6). **Runtime mode is set by the spike (R3/R4):** (a) one warm `bin/k3 --chat --no-think` child with `/reset` between items, which re-prefills the whole prompt per item; or (b) one batch process per item, `--incremental --load-state prefix.state`, prefilling only the suffix. (b) pays a cold trunk load per item (≈ `--trunk-gb` ÷ disk bandwidth, ~5 s at 24 GB) and needs the XTML chat template rendered by us and passed as `--ids`, because batch `--prompt` is a raw continuation. Pick (b) if the spike shows suffix-only prefill saves more than it costs | C1 |
| C3 | `scripts/k3/teacher_rank.py` | Phase A and Phase B drivers. Resumable by context hash. Text-free manifests | C2, :8741, chat.db, memory.db |
| C4 | `scripts/k3/calibrate.py` | Pre-registered gate (§7.2), rolling re-check | C3 output only |
| C5 | Gate edits in `scripts/blind_ab/judge_to_dpo.py` (`ALLOWED_JUDGE_FAMILIES`) and `scripts/build_v6_preference_corpus.py` (`ADMITTED`) | Admit `kimi-k3` / `k3-teacher` **only while** the calibration file reads PASS and is fresh | C4 |
| C6 | `ai.human.k3-teacher.plist` + doctor/sensor check | Schedules the phases. Alerts on 2 consecutive zero-item nights | C3 |

## 4. Data flow

**Context equals production, minus what can't be reproduced offline.**
- The prompt comes from `human reply-prompt` (`src/agent/reply_prompt.c`) under the
  service plist's `HU_*` env, which is the same mechanism `gen_onpolicy_rejected.py`
  already uses.
- `reply-prompt` omits memory/graph recall (`include/human/agent/reply_prompt.h`), so
  recall is added from `human memory ground <contact> <message>`
  (`src/app/cli_commands.c:512`). That calls `hu_graph_ground_compose`, the same
  function the live path calls at `src/agent/agent_turn.c:1101`, with the same loader
  settings (8 items, 4000 bytes).
- Both GLM and K3 see the same system + recall + turns.

**Per item.**
1. Select 1:1 moments where Seth replied, within the last **72 h**, reusing
   `gen_onpolicy_rejected.collect()` and its guards.
2. Sample **3** GLM candidates at temperature 0.7.
3. Build 4 options (3 GLM + Seth's real reply), shuffle with a seeded RNG, label
   A–D, and record the real reply's index.
4. K3 returns strict JSON: `{"rank":["C","A","D","B"],"confidence":"low|medium|high"}`.
5. Append a row: context hash, options, real index, ranking, confidence, recall
   byte count, leak flag, seconds, pauses.

**Prompt layout for K3** (cost-driven, §9): a fixed prefix (rubric plus persona
head) is prefilled once per night and saved with `--save-state`. Each item then
prefills only its suffix (recall, turns, candidates) after `--load-state`. This
requires C2 runtime mode (b) (§3). Because the prefix must be identical across items,
anything contact-specific (the contact profile from `reply-prompt`) moves into the
suffix, and only the rubric plus the persona head stay in the cached prefix.

**Temporal leak.** `hu_graph_ground_compose` has no as-of parameter, so past turns
get today's memory, which may include facts extracted from the reply being judged.
Defenses in v1:
- the 72 h recency window;
- the **leak filter**: drop the item from calibration if any content-word 3-gram of
  the real reply appears in the recall (stop-word-only overlaps don't count; drops
  are counted, and calibration refuses to report if more than 30% are dropped);
- the **no-recall control arm** (§7.2).

A true point-in-time fix could use the relation event windows in
`include/human/memory/graph_state.h` (`hu_graph_state_is_current(r, as_of_ms)`). That
is UNCONFIRMED: it's not yet verified whether relations record when they were
*learned* as opposed to when the event *happened*. v1 doesn't depend on it.

## 5. The rubric

> Which reply is most likely what Seth actually sent, given the persona, what is
> remembered about this person, and the conversation? A reply that invents details
> not in the recall is worse than a generic one. Length is not quality. Rank all
> four, best first, and state your confidence.

The target is authorship, not specificity. Seth's real replies are sometimes "lol ok",
and a specificity-only rubric would mark them wrong, so the gate would end up
measuring the rubric instead of recognition. The hallucination clause is there
because proxies for specificity and factuality can be gamed
([2411.16638](https://arxiv.org/abs/2411.16638)) and memory errors start at
extraction and spread downstream ([2511.03506](https://arxiv.org/abs/2511.03506)).

**Think mode is set by the spike.** Explicit reasoning improves judge accuracy by
about 10 points at under 2× compute ([2509.13332](https://arxiv.org/abs/2509.13332)).
On this engine, though, thinking tokens are decode tokens at seconds each, and one
measured 5-word answer spent 119 of 150 tokens in the think block (README). Default
is `--no-think`. `--thinking-effort low` is enabled only if spike throughput leaves
room for it and a 20-item A/B on the calibration set shows a top-1 gain.

## 6. Scheduling and sharing RAM

| Phase | Resource | Window | Guard |
|---|---|---|---|
| A: sample | :8741 at batch priority | 20:00, one short job | refuses 02–05, refuses if :8741 is down |
| B: rank | CPU + ~36 GB RAM, no GPU | 22:30–01:45 and 05:15–08:00, hard deadline, resumable | refuses 02–05 |

- **RAM budget:** 128 GB − GLM **61 GB** (`footprint`, measured 2026-09-26) − ~12–15 GB
  for the OS and apps ≈ 50 GB. The hard cap for K3 is **~36 GB**
  (`--trunk-gb 24 --cache-gb 8` + buffers; peak RSS confirmed by the spike). Never
  `--preset auto`, which sizes itself from free RAM and can crowd out GLM overnight.
- **Memory guard:** `memory_pressure` every 30 s. At *warn*, SIGSTOP K3; at
  *critical*, kill it and record the reason.
- **Live preemption:** poll `:8741/health/queue` every 2 s. If `live_waiting > 0` or
  a live request is active, SIGSTOP K3; SIGCONT it 20 s after the queue goes idle.
  K3 runs `nice 10`.
- **Staying visible:** every run writes a text-free manifest (items, pauses, seconds
  paused, peak RSS, s/item, exit reason). The doctor/sensor check flags **0 items for
  2 consecutive nights**. The RL pipeline once went dark for 53 days unnoticed, and
  this lane must not repeat that.

## 7. Gates

### 7.1 Premise check (first 2 nights of Phase A, measurement only)

The lane can only teach naming if GLM sometimes names things. The 2026-09-22
analysis attributes the specificity gap to an extractor that stores topic phrases
("adventure", "all the things") rather than named entities.
- **Recall has names:** share of items whose grounding contains at least one
  capitalized contact or entity name.
- **Candidate spread:** share of items where the length-matched
  `specificity_score.py` rates the 3 candidates differently.
- **Pre-registered:** spread below **30%** → pause the lane. The recommendation
  then becomes the extractor fix (store named entities), done first.

### 7.2 Calibration gate (pre-registered)

Written to `docs/evaluation/k3_judge_preregistration.json` and committed **before**
the first calibration item is ranked.

| Rule | Value | Basis |
|---|---|---|
| Items | n ≥ **80** leak-filtered | exact binomial: power **0.93** at true top-1 = 0.45 |
| Pass | real reply ranked first **≥ 30/80 (38%)**, one-sided exact binomial vs 0.25, **α = 0.01** | computed 2026-09-26 (29/80 fails) |
| Beats the free judge | K3's top-1 ≥ GLM's top-1 as a judge on the same items (same prompt, run in Phase A) | if GLM ranks as well, K3 adds nothing; self-judging also carries self-preference |
| Position bias | per-item shuffle; distribution of chosen letters χ² p > 0.01 | [2406.07791](https://arxiv.org/abs/2406.07791): position bias scales with how close the candidates are |
| Order consistency | 15% of items re-ranked with the order reversed; agreement reported | single permutation for cost; this subset measures the noise |
| Parse failures | < 5% | — |
| Leak control | 20% of items ranked without recall; reported as a diagnostic | [2608.02985](https://arxiv.org/abs/2608.02985): an apparent advantage can be leakage or recency |

- **Rolling re-check:** admission reads the last 100 items. Falling below the
  threshold revokes admission of **new** pairs.
- **Prior:** LLM judges are poor to moderate at telling humans from AI in dialogue.
  GPT-4 was judged human more often than humans
  ([2407.08853](https://arxiv.org/abs/2407.08853)); frontier judges reach 76–78%
  pairwise ([2606.21844](https://arxiv.org/abs/2606.21844)). Human raters already
  detect GLM at only 0.225. **Failing this gate is a plausible, legitimate outcome.**

### 7.3 Pair emission (only while admitted)

- `chosen` = top-ranked GLM candidate, `rejected` = bottom-ranked GLM candidate.
  Seth's real reply never appears in a teacher pair (`gen_onpolicy_rejected.py`
  already covers Seth-vs-GLM).
- Drop items where the real reply was ranked **last**.
- Drop items with `confidence: low`. Confidence-aware weighting beats uniform DPO
  ([2511.07691](https://arxiv.org/abs/2511.07691)); the mlx trainers can't weight
  rows, so this is the hard-filter approximation. Verbalized confidence is the more
  robust signal on post-2025 models ([2609.10996](https://arxiv.org/abs/2609.10996)).
- **Length balance:** if chosen is the longer reply in more than 60% of pairs,
  subsample to 50/50. DPO exploits length ([2409.06411](https://arxiv.org/abs/2409.06411),
  [2403.19159](https://arxiv.org/abs/2403.19159)), and terseness is part of the voice
  that passed.
- The full ranking is stored. v1 trains DPO on top-vs-bottom because
  `mlx_lm_lora` / `mlx_tune` support that. Ranked-choice objectives consistently beat
  pairwise baselines ([2510.23631](https://arxiv.org/abs/2510.23631), RCPO), so the
  data is kept for a v2 trainer.

### 7.4 Promotion (K3 never grades its own student)

- Existing chain, unchanged: `adapter_is_real` → held-out loss →
  `score_candidate_offline` → `authorship_promotion_gate` → `m3_promote` (rollback).
- **Added, length drift:** BLOCK if the held-out median reply length is more than
  **+15%** over v5's.
- **Added, no circular evaluation:** K3 may not evaluate any adapter trained on K3
  pairs.
- **Final decision, human blind A/B:** specificity must improve (length-matched
  `specificity_score.py`) and detection must not exceed 0.225 by more than the CI.
- **Iteration:** on-policy preference learning converges fast in rounds
  ([2601.08421](https://arxiv.org/abs/2601.08421)). Each round re-samples from the
  newly *promoted* adapter, with **at most 3 rounds**, and at most one candidate
  every ~2 weeks because the human rating sheet is already the program's bottleneck.

## 8. Testing

**Unit (pytest, following `scripts/test_gen_onpolicy_rejected.py`; clock injected via `main(argv, now=)`):**
- **k3_server** (stub `bin/k3`): returns only `<response>`; `/reset` between
  requests; concurrent requests serialized; non-loopback bind refused; argv never
  contains `--history`; fake `/health/queue` `live_waiting=1` → SIGSTOP, idle + 20 s
  → SIGCONT; injected `critical` pressure → child killed and reason recorded.
- **teacher_rank:** seeded shuffle is deterministic and records the real index;
  resume skips ranked hashes; refuses 02–05; ranking parse accepts a valid
  permutation, rejects missing or duplicate letters, and uses the last JSON envelope
  (the `synthetic_judge.parse` rule).
- **Leak filter:** content 3-gram overlap → flagged; none → not flagged; stop-word-only
  overlap → not flagged.
- **calibrate:** **29/80 → FAIL, 30/80 → PASS** (pins α and chance in code); skewed
  χ² → FAIL; K3 < GLM baseline → FAIL; rolling window below threshold → admission
  revoked.
- **Pair emission:** real reply never in pairs; real ranked last → dropped; low
  confidence → dropped; length subsampling to ≤ 60% longer-chosen.
- **Gate edits:** before/after contract on the same rows. Calibration file absent,
  stale or FAIL → rejected; fresh PASS → admitted.

**Integration.**
- CI: end-to-end A → B → calibrate → pairs with the stub `bin/k3` and a stub :8741,
  synthetic data only.
- Manual real-engine smoke: 3 synthetic items with GLM live and one hand-triggered
  preemption. Record parse success, s/item, and peak RSS against the 36 GB cap.
- Wiring: grep proves the plist invokes both phases and the gate edits read
  `k3_judge_calibration.json`.

**Process.** Each task closes on a `verifier` PASS with captured output, then one
critic pass (max 2 rounds).

## 9. Throughput — measured 2026-09-26

**Prefill, not decode, dominates.** The engine hard-codes `CHUNK = 64` for batched
prefill (`src/core/k3_ops.c:707`) and fetches each unique expert once per chunk. A
17-token prompt read 200.67 GB (README); a 40-token prefill read 330.73 GB
(`docs/data/speed-2026-08.md`). At ~3.4k tokens, a 64-token chunk re-reads most
experts about 55 times. The chunk width exists only to bound a scratch buffer
(14.7 MB at 64 tokens), which is tiny next to this machine's RAM.

The spike (`~/k3spike/run_spike.sh`, 10 of 93 layers, synthetic prompts, internal
SSD, so an **optimistic bound**) measures:
- **R0:** first-step logits bit-identical at chunk 64 vs 4096?
- **R1/R2:** full ~3.4k-token prompt, chunk 64 vs 4096.
- **R3/R4:** prefix saved once, then the suffix alone.

Extrapolation: experts × 92/9, trunk × 93/10, MLA share corrected (2/10 sampled vs
24/93), disk time rescaled to 5.3 GB/s (OWC Envoy Ultra, measured read).

**Measured 2026-09-26** (M4 Max, layers 0–9, internal SSD, `--cache-gb 8`, GLM
live but idle-ish; synthetic prompts; `~/k3spike/results/`). Prefill seconds at 10
layers → full model at ×9.3–10.2:

| Run | Tokens | Prefill (10 layers) | GB read | Full model (est.) |
|---|---|---|---|---|
| R0 chunk 64 | 816 | 257.9 s | 411.2 | — |
| R0 chunk 4096 | 816 | 237.6 s | 132.5 | 37–40 min |
| R1 full prompt, chunk 64 (as shipped) | 3,793 | 1,524.7 s | 1,854.3 | **3.9–4.3 h** |
| R2 full prompt, chunk 4096 | 3,793 | 1,163.4 s | 140.4 | 3.0–3.3 h |
| R3 prefix only (save state) | 2,977 | 844.2 s | 139.3 | 2.2–2.4 h, **one-time** |
| R4 suffix after `--load-state` | 816 | 322.4 s | 134.3 | **50–55 min/item** |

- **Wide chunk is exact:** first-step logits bit-identical, chunk 64 vs 4096 (R0).
  Reads fall 3.1× (816 tok) to 13× (3,793 tok), but wall time falls only 8–24%.
- **Prefill is compute-bound, not disk-bound.** ~0.1–0.6 GB/s effective read vs
  6–9 GB/s measured trunk load; ~5.8 of 16 cores busy. The cause is in the code:
  prefill projections and expert applications run one token at a time
  (`k3_mmw` / `k3_matmul_mxfp4` inside `for (t < T)`, e.g. `src/core/k3_ops.c:865`),
  each opening its own OpenMP region. Every weight matrix streams from RAM once per
  prompt token.
- **Cached prefix works**, but the suffix pays for attending to the prefix
  (816 tokens: 237.6 s alone vs 322.4 s after a 2,977-token prefix, +36%). The
  prefix state is contact-independent, so it can be computed **once and reused
  across nights** until the rubric or persona changes.
- Decode, once prefilled: ~1.0 s/token at 10 layers (≈ 10 s/token full model, RAM
  and internal SSD). For a ~20-token ranking that adds ~3–7 min/item.

| Quantity | Value (optimistic) |
|---|---|
| min/item, best config (cached prefix + wide chunk + ~20 output tokens) | **~55–62** |
| items/night (6 h, before preemption) | **~6** |
| nights to 80 calibration items | **~12–14** |
| nights to 500 pairs | **~80+** |

**Drive rule result: FAIL** (≤ 15 min/item required; measured ~4× over). A faster
drive cannot fix a compute-bound prefill. **Do not buy the drive yet.**

**Next step (separate task, needs its own approval):** a token-tiled batched
prefill kernel in the fork (read each weight row once per block of 32–64 tokens).
Per-element summation order is unchanged, so the R0 bit-identical gate applies.
Then re-run R4. Independently, trim the per-item suffix (the synthetic suffix
carried ~330 words of "earlier context"; ~400 tokens is realistic), since prefill
scales roughly linearly with it. **If R4 is still above 15 min/item after both,
close the local-K3 lane** and record the result. The GLM-as-judge baseline (§7.2)
remains available as the cheap judge.

**Drive rule:** buy the 4 TB TB5 drive only if the best configuration comes out at
≤ **15 min/item** (≥ ~25 items/night). If that relies on the wide chunk, R0 must show
bit-identical logits, and the change goes upstream to the fork as a flag, not as a
private patch.

## 10. Prerequisites and open items

1. **Land the length-matched `specificity_score.py`** and its test. The 2026-09-22
   version (length-matched cohort, bootstrap CI, `test_specificity_score.py`) is
   **not on main**; main has only `b2dda9536`. It's needed by §7.1 and §7.4.
2. **Drive** (§9 rule): 4 TB, Thunderbolt 5. Needs 1.56 TB checkpoint + 108.81 GB
   packed trunk (README).
3. **License:** confirm the Kimi K3 license terms for using outputs to train another
   model before the first pair is emitted. One agent reported it's permitted, with a
   commercial revenue threshold; that is UNVERIFIED against the license text.
4. **Prefill chunk flag upstream** in the fork, if R0 passes.
5. **As-of recall** (§4), if the leak filter drops more than 30% of items.

## 11. Evidence ledger

Every citation above was fetched and checked against its abstract on 2026-09-26.
Two agent-proposed claims failed and were **excluded**:
- "~5 samples/prompt is optimal" ([2502.16825](https://arxiv.org/abs/2502.16825)
  doesn't say so; N=3 is a cost choice here);
- "KTO beats DPO on small, noisy data" ([2411.09539](https://arxiv.org/abs/2411.09539)
  makes no such comparison).

[2501.13956](https://arxiv.org/abs/2501.13956) (Zep) exists, but its abstract doesn't
claim point-in-time querying, so it isn't cited for that. Agent-computed figures
that were wrong and replaced:
- calibration sample size (claimed 110–150; exact binomial gives ~42 at α=0.05 and
  80 at the chosen α=0.01);
- packed trunk size (claimed 1.7 TB; README says 108.81 GB);
- per-item time (claimed ~6 min; its own inputs give ~10.7 h).
