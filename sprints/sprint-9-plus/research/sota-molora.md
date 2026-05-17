# SOTA — MoLoRA & Dynamic Adapter Routing (May 2026)

**Author:** research agent  **Date:** 2026-05-16  **Status:** Sprint 9+ grounding
**Related:** `docs/plans/2026-05-11-init-02-molora-channels.md`, `sprints/sprint-7/audit.md` (US-7.8 DELIVERED_WITH_DRIFT), `src/ml/molora.c` (145 LOC static router), `include/human/ml/molora.h` (`HU_MOLORA_MAX_CHANNELS = 16`).

> **Caveat.** Live WebSearch was not available; citations are sourced from
> Init #02 §11.3 (assembled end-April 2026) plus Init #06. Any May-2026-only
> paper would need a follow-on search before being load-bearing.

## 1 · May 2026 MoLoRA rankings (top 5)

| Rank | Paper | arXiv | Key contribution |
|------|-------|-------|------------------|
| 1 | **LD-MoLE** (Shen et al., ICLR 2026) | 2509.25684 | Differentiable Sparsegen routing, token- + layer-level adaptive sparsity on Qwen3-1.7B / Llama-3.2-3B. SOTA on per-task gains at ≤ 1.3× single-adapter latency. |
| 2 | **DynMoLE** (Wu, Zhang et al., 2025) | 2504.00661 | Tsallis-entropy hybrid routing → dynamic top-k. Beats fixed-k MoLE by 1.5–4 pts; doesn't touch base forward pass. |
| 3 | **SAMoRA** (Wang et al., ACL 2026 Findings) | 2604.19048 | Semantic-aware routing + task-adaptive scaling. **Most relevant to per-persona routing** — Init #02 borrows its semantic feature-vector design. |
| 4 | **MoLE / MoLoRA** (Wu, Huang, Wei, ICLR 2024) | 2404.13628 | Foundational shape: N independent LoRAs + hierarchical gate. h-uman's static router is a degenerate slice. |
| 5 | **X-LoRA** (Buehler & Buehler, 2024) | 2402.07148 | Existence proof for tiny *external* MLP gate (router outside base model). |

**Adjacent:** MixLoRA (2404.15159) — token-routing needs base modifications;
LoraHub (2307.13269) — cold-start LoRA composition; Mixture-of-Subspaces
LoRA (2406.11909) — orthogonal to routing.

## 2 · Routing granularity at our scale

| Granularity | Win condition | Cost | Source |
|-------------|---------------|------|--------|
| **Per-turn** (channel + macro-mode) | Channel ≈ tone | Zero forward-pass cost | Sprint 7 static; SAMoRA-lite |
| **Per-token** (LD-MoLE) | Mid-turn style shifts | Modifies base; +20–30% latency | 2509.25684 |
| **Per-layer** (DynMoLE) | Different layers want different experts | Gate per layer, no per-token | 2504.00661 |
| **Per-task** (LoraHub) | Cold-start unseen tasks | Negligible at inference | 2307.13269 |

**For h-uman:** per-turn is the right first cut — our thesis is "channel tone
is the dominant signal." If A/B shows a single global adapter ties per-channel
adapters, per-token routing won't save us.

## 3 · Joint vs sequential training

Two clean camps: **joint** (MoLE-original, DynMoLE — higher fidelity, must
retrain mixture for any change) vs **sequential/additive** (LoraHub, SAMoRA
partially — cheap, new channels are additive). **h-uman recommendation:
sequential.** Init #02 §1 already commits: "new channel = new expert =
additive, isolated training; existing channels' weights unchanged." Joint
training would force a W14 idle scheduler rework — out of scope.

## 4 · Persona-specific MoLoRA — published evidence

**Thin.** No paper publishes per-persona LoRA routing on a real product
surface (as of Init #02 cut). SAMoRA's semantic routing transfers
architecturally but uses task embeddings, not persona embeddings. **Sprint 9
is whitespace.** Sprint 8 NLL backend (US-8.2) is the prerequisite — without
a real loss, the A/B is theatre.

## 5 · Inference cost — N small vs 1 wide

- **N small** (rank 8, top-3 active): ~1.1× base inference. `llama_set_adapters_lora(ctx, A[], n, w[])` batches the apply step.
- **1 wide** (rank 32–64, same total bytes): ~1.05× base. Cheaper but Telegram-casual bleeds into Slack unless rank is huge.
- **LD-MoLE §4.2 inflection:** k=3 active rank-8 experts is the sweet spot. Below k=3, mixture wins; above k=3, wider single starts winning on latency. **`HU_MOLORA_MAX_ACTIVE = 3` is correct.**

Memory: 8 channels × 16 MB = 128 MB resident. Fine for desktop; tight for
RPi. Cache-eviction path is post-Sprint-9.

## 6 · Cold-start adapters

LoraHub's core contribution (2307.13269): sparse linear combination of
existing LoRAs as warm-start for a new task. For h-uman: when a user adds
WhatsApp, initialize as blend of (Telegram + iMessage) experts, then warm-start
channel-specific training. LD-MoLE §5.1 reports ~40% wall-clock saving on
Qwen3-1.7B.

## 7 · Sparse routing — K of N

`HU_MOLORA_MAX_ACTIVE = 3` already locked in Init #02. Open question is
**dynamic** sparsity (DynMoLE Tsallis-entropy, +1.5 pts) vs fixed top-3.
~30 LOC delta in the router once Phase 2 lands.

## 8 · Recommended next phase for h-uman MoLoRA

**Sprint 7 shipped Phase 1: static channel-id → adapter lookup (145 LOC,
strncmp exact match).** Known drift: FU-7.8.a (basename collision, P1),
FU-7.8.e (OFF-build symbol absence, P2).

- **Phase 2 (Sprint 9):** learned MLP router + sequential per-channel
  training. Init #02 §3 specifies the shape: 4.3 KB FP32 router, one hidden
  layer, input = `[channel_one_hot, message_class_one_hot, macro_mode_one_hot]`,
  output = softmax over 8 slots. W14 trains it offline. A/B vs Sprint 7 static
  via Sprint 8 NLL backend.
- **Phase 3 (Sprint 10+):** DynMoLE-style dynamic top-k. Only if Phase 2
  promotes.
- **Phase 4 (deferred indefinitely):** LD-MoLE token-level. Requires
  modifying base forward pass; violates `hu_provider_t` boundary.

## 9 · Sprint 9 stories with citation grounding

| ID | Story | Citation | AC sketch |
|---|---|---|---|
| **US-9.1** | Fix FU-7.8.a (basename) + FU-7.8.e (OFF-build) | Sprint 7 followups | Adapter ids stored as full path or hashed; CI confirms `HU_ENABLE_MOLORA=OFF` strips symbols |
| **US-9.2** | Per-channel bank construction from W12 memory | 2604.19048 §3 | JSONL bank per Tier-1 channel; PII redact via `hu_pii_redact` |
| **US-9.3** | Sequential per-channel LoRA training via W14 | 2307.13269 (additive) | `lora-channel-train --channel telegram`; existing channels untouched |
| **US-9.4** | Cold-start blend for new channels | 2307.13269 §3 | New channel initialized as weighted blend of 2 most-similar existing adapters |
| **US-9.5** | Learned MLP router (4.3 KB FP32) | 2404.13628 + 2604.19048 | `router.bin` with magic header; falls back to static lookup if absent |
| **US-9.6** | W14 router training job | 2404.13628 §4 (gate loss) | `HU_JOB_MOLORA_ROUTER_TRAIN`; consumes (features, target-slot) tuples from Sprint 7 logger |
| **US-9.7** | A/B gate: learned vs static via Sprint 8 NLL | 2509.25684 §4.2 | `lora-ab --router learned --baseline static` must beat baseline by ≥ 1 stderr on Tier-1 |
| **US-9.8** | (Stretch) Dynamic top-k via Tsallis entropy | 2504.00661 | Replace fixed-3 cap with entropy-derived k ∈ {1,2,3}; deferred if US-9.5–7 don't promote |

**Hard descope rule:** if US-9.5 + US-9.7 ship and the learned router
**does not** beat static by ≥ 1 stderr, *do not* proceed to US-9.8. Write the
negative result, drop the initiative back to "static is sufficient at our
scale," and reprioritize toward M4 (Ship to Users). Consistent with Init #02
§9 D7 defer condition.

## 10 · Open research questions for follow-on

1. **Per-person vs per-channel.** Init #02 assumes channel is dominant. At
   M4 100-DAU scale, can we observe per-person variance large enough to
   justify per-user experts? Need data first.
2. **W14 concurrent training.** Scheduler assumes one run at a time. Parallel
   overnight retraining of 4 Tier-1 channels needs queue + GPU partitioning.
3. **Verifier-driven router training (Init #05 crossover).** Could the
   router be trained against verifier-TTT signals rather than observed-channel
   ground truth? Speculative; out of Sprint 9.

---

RESULT_research=READY
