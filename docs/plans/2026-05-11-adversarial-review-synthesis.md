---
title: "SOTA-2026 adversarial review synthesis — go/no-go matrix"
created: 2026-05-11
status: closed
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-adversarial-review-critic.md
  - 2026-05-11-adversarial-review-api-contracts.md
  - 2026-05-11-adversarial-review-security.md
  - 2026-05-11-init-09-memory-trust-tiers.md
  - 2026-05-11-init-04-mlx-qwen3-provider.md
  - 2026-05-11-init-08-federated-lora.md
  - 2026-05-11-init-10-episode-storage-sleep-consolidation.md
  - 2026-05-11-init-12-mcp-server-mode.md
last_audit: 2026-05-25
---

# SOTA-2026 adversarial review synthesis

The three reviewers (critic, api-contract-watcher, security-reviewer) finished. This document consolidates their findings into one **go/no-go matrix** with concrete fixes per finding and a clear sequence: **what closes before S1 dispatch**, **what gets baked into S1 implementer prompts**, and **what becomes a design-doc revision ticket for the deferred initiatives**.

The full reviewer outputs live at:
- `docs/plans/2026-05-11-adversarial-review-critic.md` — 4 BLOCKER findings
- `docs/plans/2026-05-11-adversarial-review-api-contracts.md` — 6 ABI-BREAKING findings, 37 name-collision call sites
- `docs/plans/2026-05-11-adversarial-review-security.md` — 4 CRITICAL findings + 10 MINJA bypass categories

## Top-line verdict

**S1 dispatch holds pending precondition fixes.** Five concrete corrections close all 4 BLOCKERs and unblock S1. None of them require new design work — they are clarifications and bug fixes against existing design docs. Estimated wall-clock to close: ~30 minutes of edits.

After fixes land, the 5 S1 candidate initiatives all clear go-with-fix:

| # | Initiative | Verdict | Conditions to GO |
|---|------------|---------|-------------------|
| **09** | memory-trust-tiers | **GO-WITH-FIX** | Migration default = `THIRD_PARTY` (B2); all 4 ingest call sites patched (B3/A1); MINJA detector broadened beyond 10 patterns (security); trust-tier ordinal convention locked in master (B1). |
| **04** | mlx-qwen3-provider | **GO-WITH-FIX** | `$HUMAN_MLX_QWEN3_HELPER` env-var override gated to `HU_IS_TEST` builds only (S2 in security review). |
| **14** | public-benchmarks | **GO** | None — pure additive, no security surface, no ABI risk. |
| **01** | activation-steering (prompt-side half only in S1) | **GO** | None — string splice into existing frontier_prompt bundle. |
| **11-typing** | typing simulation half of #11 | **GO** | Confirm typing half does NOT claim any new `hu_job_kind_t` enum value (gate half does; typing half should not). |

The deferred-to-S2/S3 initiatives have **design-doc revisions** required before they ship — separately tracked below.

---

## BLOCKER findings (must close before any S1 dispatch)

### B1 — Trust-tier ordinal inversion between #09 and #10

**Problem.** Init #09 encodes `HU_TRUST_USER_DIRECT = 4` (high number = high trust, like Linux capability constants). Init #10's `hu_episode_t.trust_tier` field comment says `0 = user-direct` (low number = high trust). If both ship as written, any cross-initiative code reading one and passing to the other applies **inverted** filtering silently — third-party group-chat memories would be ranked as user-direct.

**Fix.** Lock the convention in the master coordinator **before #10 reaches design done**:

> **TRUST-TIER ORDINAL CONVENTION** (canonical). Higher integer = more trusted. `HU_TRUST_UNTRUSTED = 0`, `HU_TRUST_THIRD_PARTY = 1`, `HU_TRUST_FIRST_PARTY = 2`, `HU_TRUST_PERSONA_DERIVED = 3`, `HU_TRUST_USER_DIRECT = 4`. Comparisons are `>=` for "is at least this trusted". Init #10's design doc must be patched to match before its design-done flip.

**Action.** Apply to master plan. Init #10 design doc gets a one-line patch when it enters S2 design refresh.

### B2 — Init #09 migration upgrades poisoned facts to FIRST_PARTY trust

**Problem.** Init #09's design proposes `UPDATE memories SET trust_tier = 2 (FIRST_PARTY)` for all pre-existing rows. Any memory poisoned by a MINJA-style attack **before** the migration runs gets *upgraded* to the second-highest trust tier — harder to overwrite, less likely to be quarantined, immune to the verifier rejection that low-trust facts get. The security feature actively worsens the threat it claims to fix.

**Fix.** Migration default = `HU_TRUST_THIRD_PARTY (1)` for any row whose `provenance.source_channel` does NOT match a CLI/direct-input channel (`cli`, `tui`, `web_ui`). User-direct migration is reserved for rows where the source channel is verifiable as user-typed. Old memories from group chats, RSS feeds, Gmail, etc. all get `THIRD_PARTY`.

For safety-paranoid users, ship a `human memory audit --strict` command that bulk-relabels all pre-migration rows to `UNTRUSTED (0)` and re-promotes them only on explicit user confirmation.

**Action.** Patch init-09 design doc §Migration before S1 dispatch.

### B3 / A1 — `hu_personal_model_ingest` 4 live call sites, design patches only 2

**Problem.** Init #09 adds a required `const hu_provenance_t *prov` parameter to `hu_personal_model_ingest`. The design names only 2 patches (`agent_turn.c:951`, `agent_stream.c:373`). The contract reviewer found **4 live call sites** (`src/memory/personal_model.c:1662`, `src/agent/agent_turn.c:951`, `src/agent/agent_stream.c:373`, `src/agent/agent_stream.c:2585`). The build will fail on first compile.

**Fix.** Patch init-09 design doc §Code violations to enumerate all 4 call sites with their correct provenance derivation:

| File:line | Current call | New call |
|-----------|-------------|----------|
| `agent_turn.c:951` | `hu_personal_model_ingest(pm, text, true)` | `hu_personal_model_ingest(pm, text, true, &(hu_provenance_t){.source = HU_PROV_USER_DIRECT, .channel_id = ctx->channel_id, .ts = now})` |
| `agent_stream.c:373` | same shape | same shape |
| `agent_stream.c:2585` | same shape | same shape |
| `personal_model.c:1662` | internal self-call | derive provenance from caller context; if NULL, default to `HU_PROV_FIRST_PARTY` |

**Action.** Patch init-09 design doc §Code violations + §File list before S1 dispatch. The S1 implementer prompt will execute the patch.

### B4 — `hu_episode_t` ODR violation already shipping in main

**Problem.** `include/human/agent/episodic.h` and `include/human/memory/deep_memory.h` already define **incompatible** `hu_episode_t` structs. The contract reviewer found `include/human/memory/episodic.h` as a third definition with another layout. This is a **live defect** independent of any SOTA-2026 initiative — Init #10 would compound it by adding a fourth definition.

**Fix.** W7 cleanup rename slice is elevated from "optional in S1" to **REQUIRED in S1 week 0**. Renames:
- `agent/episodic.h::hu_episode_t` → `hu_session_episode_t` (agent-side session turn)
- `memory/deep_memory.h::hu_episode_t` → `hu_deep_episode_t` (memory consolidation snapshot)
- `memory/episodic.h::hu_episode_t` → either delete (if dead) or rename `hu_episodic_memory_record_t`

Init #10's `hu_episode_t` claim is then valid in S2 once the slot is free.

**Action.** Master plan §Implied build order updated to make W7 slice REQUIRED week 0 (not optional).

---

## API-contract findings (most fold into BLOCKER fixes; some are S2+)

### A2 — `hu_job_kind_t` enum ordinal collision across 4 initiatives

**Problem.** Inits #02, #05, #10, #11 all independently propose new enum values "appended before `HU_JOB_KIND_MAX`". All four would receive the same ordinal at compile time, silently routing wrong runner functions to wrong job kinds.

**Fix.** Publish a **`hu_job_kind_t` enum allocation table** in the master coordinator. Each initiative claims a specific ordinal:

| Ordinal | Symbol | Owner | Status |
|---------|--------|-------|--------|
| 10 | `HU_JOB_MOLORA_ROUTER_TRAIN` | #02 | reserved |
| 11 | `HU_JOB_TTT_DRIFT_EVAL` | #05 | reserved |
| 12 | `HU_JOB_CONSOLIDATE_NREM` | #10 | reserved |
| 13 | `HU_JOB_CONSOLIDATE_REM` | #10 | reserved |
| 14 | `HU_JOB_PROACTIVITY_RECHECK` | #11 (gate half) | reserved |

When an initiative goes to design-done, it must reference this table. New ordinals append; **never insert mid-enum**.

**Action.** Apply to master plan. Does not affect S1 directly (typing half of #11 claims no ordinal) but unblocks S2.

### A3 — Init #12 removes `hu_mcp_host_*` family with no shim

**Problem.** Init #12 deletes `hu_mcp_host_create`, `hu_mcp_host_run`, `hu_mcp_host_destroy`. 5 live call sites in `src/main.c:2407-2421` will fail to link.

**Fix.** Init #12 design doc §Rename slice expanded to: keep `hu_mcp_host_*` symbols as deprecated shims for one minor version (1.x → forwarded to the renamed `hu_mcp_engine_*`); add `__attribute__((deprecated("rename to hu_mcp_engine_*")))`; remove in the next major. Same pattern as the Phase 0 `hu_dpo_train_step` → `hu_dpo_judge_step` rename.

**Action.** Init #12 is deferred to S2; revision ticket logged below. Does not block S1.

### A4 — `hu_memory_entry_t` struct layout change

**Problem.** Init #09 inserts 2 new fields (`trust_tier`, `provenance`) into `hu_memory_entry_t`. 40+ `sizeof()` consumers across 15+ files.

**Fix.** Verify (during S1 #09 implementation) that all `sizeof()` consumers are either:
- `sizeof(hu_memory_entry_t)` used as a buffer-alloc size (safe — struct grows, alloc grows)
- `sizeof(struct hu_memory_entry_t)` used in a serialization path (NOT safe — version-stamped serializers must be audited)

The implementer prompt must include "grep for `sizeof(hu_memory_entry_t)` and audit each occurrence for buffer-vs-serialization use" as a checklist item.

**Action.** Bake into S1 #09 implementer prompt. Likely safe (most consumers are buffer-alloc) but must be verified.

### A5 — `#ifdef HU_ENABLE_TTT`-gated vtable field in `hu_learner_vtable_t`

**Problem.** Init #05 adds three new function pointers to `hu_learner_vtable_t` behind `#ifdef HU_ENABLE_TTT`. If different TUs are compiled with mixed flag settings, the struct has different sizes — ODR violation, undefined behavior.

**Fix.** Init #05 design doc §Vtable surface revised: TTT pointers are **always present** in the vtable, set to `NULL` when `HU_ENABLE_TTT` is off. Dispatcher returns `HU_ERR_NOT_SUPPORTED` for `NULL` pointers (same pattern as cloud providers ignoring `apply_steering`).

**Action.** Init #05 is deferred to S2; revision ticket logged below. Does not block S1.

---

## Security findings

### S-CRIT-1 — Init #08 SECAGG over GF(2^8) is mathematically wrong

**Problem.** Init #08's `SECAGG_SHAMIR` uses Shamir secret sharing over GF(2^8). XOR-aggregation produces corrupted adapters on every SecAgg round — the aggregate is bitwise XOR of gradient tensors, not floating-point average.

**Fix.** Init #08 is deferred to S3 anyway (adapter-format heterogeneity gate). Design doc revision ticket: replace Shamir/GF(2^8) with **additive secret sharing over a prime field** large enough to hold quantized gradient sums (`GF(p)` with p ≈ 2^61, gradients quantized to int32), OR adopt **Joye-Libert-style additive masks** (each peer adds noise summing to zero across peers, exact aggregation, no field issues).

**Action.** Init #08 design-doc revision ticket logged. Does not block S1.

### S-CRIT-2 — `$HUMAN_MLX_QWEN3_HELPER` env-var override is a code-injection path

**Problem.** Init #04 supports an env-var override for the helper subprocess path. Any user-level process can set this env var and silently inject code into the chat inference loop.

**Fix.** Gate the env-var override to `HU_IS_TEST` builds only:

```c
#ifdef HU_IS_TEST
    const char *override = getenv("HUMAN_MLX_QWEN3_HELPER");
    if (override) path = override;
#endif
```

Production builds use only the binary-relative path computed at compile time. Plus an absolute-path integrity check (mode bits, owner uid) before exec.

**Action.** Bake into S1 #04 implementer prompt as an explicit security checklist item. Implementer must add a deterministic test that verifies the override is rejected in non-test builds.

### S-CRIT-3 — Init #05 TTT has no trust-tier gate before #09 ships

**Problem.** Init #05 TTT trains LoRA on conversation pairs. Without Init #09's trust filtering, a crafted group-chat message can drive a gradient step against the user's persona adapter.

**Fix.** Init #05 design doc §Trigger gates updated to require: every TTT input pair's underlying memory must have `trust_tier >= HU_TRUST_PERSONA_DERIVED (3)`. Third-party-sourced facts are NEVER allowed to drive gradient updates. Plus a hard assertion in the learner: if Init #09 is not built into the binary, TTT refuses to run.

**Action.** Init #05 design-doc revision ticket logged. Does not block S1 (since #05 is deferred to S2).

### S-CRIT-4 — Most-embarrassing scenario: MCP peer-driven LoRA replacement

**Problem.** Init #12 exposes `call_tool` to paired MCP peers (Cursor, Claude Code). A malicious comment in a reviewed PR could prompt-inject Cursor into calling our `replace_lora_adapter` tool. The attack lands silently in the audit log as `peer=cursor ok=true` — the user's personal model is permanently hijacked.

**Fix.** Init #12 design doc §Tool exposure policy revised: **mutating tools (any tool that writes to `hu_personal_model_t`, replaces an adapter, modifies persona overlay, or writes to memory) are NEVER exposed via MCP**, regardless of `mcp_consent.json` settings. MCP peers see read-only resources only. The `call_tool` surface limits to side-effect-free queries (`describe_persona`, `recall_facts_about`, etc.). User-confirming UX (toast + explicit click) is required for any tool that would change state.

**Action.** Init #12 design-doc revision ticket logged. Does not block S1.

### S-MAJOR — MINJA detector has 10 bypass categories

**Problem.** Init #09's MINJA detector is a 10-pattern English-language matcher. The security reviewer enumerated 10 bypass categories: paraphrase, base64-encoded payload, foreign-language injection, Unicode lookalikes, leetspeak, multi-turn split injection, indirect-reference injection ("as we agreed"), social-engineering compliance ("I'm helping you"), reverse-psychology framing, instruction-via-quotation.

**Fix.** Init #09 design doc §MINJA detector revised:
1. Pattern set broadened to ≥30 patterns covering all 10 bypass categories.
2. Detector runs on **normalized** content: Unicode-NFKC + lowercase + leetspeak undo + (optional) base64-decode pass.
3. Second-line defense: facts extracted from THIRD_PARTY content are NEVER auto-applied — they go to a `~/.human/pending_facts.log` for explicit user review.
4. Foreign-language detection: any incoming third-party message in a language other than the user's configured locales gets `THIRD_PARTY` MAX, no exceptions, no fact extraction.

**Action.** Patch init-09 design doc §MINJA detector before S1 dispatch.

---

## Precondition fixes that MUST close before S1 implementer dispatch

| Fix | Where | Effort |
|-----|-------|--------|
| Lock trust-tier ordinal convention (`USER_DIRECT=4`, etc.) | Master plan §Synthesis (new subsection) | 5 min |
| Publish `hu_job_kind_t` enum allocation table | Master plan §Synthesis (new subsection) | 5 min |
| Elevate W7 `hu_episode_t` ODR cleanup to S1 week 0 REQUIRED | Master plan §Implied build order | 2 min |
| Init #09: migration default = `THIRD_PARTY` not `FIRST_PARTY` | `init-09-memory-trust-tiers.md` §Migration | 5 min |
| Init #09: all 4 ingest call sites enumerated with provenance derivation | `init-09-memory-trust-tiers.md` §Code violations | 10 min |
| Init #09: MINJA detector broadened to ≥30 patterns + normalization + pending_facts.log fallback | `init-09-memory-trust-tiers.md` §MINJA detector | 10 min |
| Reference this synthesis from master plan §Adversarial review | Master plan | 2 min |

Total: ~40 minutes of edits. After these land, S1 implementer dispatch is unblocked.

## Design-doc revisions tracked for deferred initiatives

These are NOT precondition for S1. They are tracked here so they get applied before the corresponding initiative ships in S2/S3.

| Ticket | Initiative | Revision required | Owner |
|--------|------------|-------------------|-------|
| REV-05-01 | #05 TTT | Vtable fields always-present, NULL when `HU_ENABLE_TTT` off | S2 implementer |
| REV-05-02 | #05 TTT | Trust-tier gate `>= HU_TRUST_PERSONA_DERIVED` on every input pair | S2 implementer |
| REV-08-01 | #08 federated-lora | Replace SECAGG_SHAMIR over GF(2^8) with additive masks / GF(p≈2^61) | S3 design refresh |
| REV-10-01 | #10 episode-storage | Trust-tier ordinal aligned with #09 convention | S2 implementer |
| REV-10-02 | #10 episode-storage | Pre-unlock retrieval policy: block proactivity if `encrypt_at_rest && locked` | S2 implementer |
| REV-12-01 | #12 mcp-server | `hu_mcp_host_*` deprecation shims | S2 implementer |
| REV-12-02 | #12 mcp-server | Mutating tools NEVER exposed via MCP (read-only resources only) | S2 implementer |
| REV-13-01 | #13 kv-compression | Phase-5 real-activation side-channel check before commit | S2/S3 implementer |

## S1 implementer prompt checklist additions

When the S1 implementer fleet dispatches, each implementer prompt must include the relevant baked-in checks:

**#09 trust-tiers implementer**:
- Migration default: `THIRD_PARTY` not `FIRST_PARTY`
- Patch all 4 ingest call sites (`personal_model.c:1662`, `agent_turn.c:951`, `agent_stream.c:373`, `agent_stream.c:2585`)
- MINJA detector ≥30 patterns + Unicode normalization + non-locale-language reject
- `sizeof(hu_memory_entry_t)` audit (grep + classify each occurrence)
- Trust-tier ordinal convention: `USER_DIRECT=4` (high integer = trusted)

**#04 mlx-qwen3 implementer**:
- `$HUMAN_MLX_QWEN3_HELPER` env-var override gated to `HU_IS_TEST`
- Absolute-path integrity check (mode bits + owner uid) before exec
- Test that override is rejected in non-test builds

**#14 benchmarks implementer**: no special checklist.

**#01 activation-steering implementer**: no special checklist (prompt-side half is a string splice).

**#11-typing implementer**:
- Confirm zero new `hu_job_kind_t` ordinal claimed (gate half claims `HU_JOB_PROACTIVITY_RECHECK = 14`; typing half claims none)

## What this synthesis does NOT cover

- Init-06 (SimPO/ORPO/GRPO-2) design still in flight at the time of this writing. When it lands, it gets the same adversarial pass against the locked trust-tier convention + `hu_job_kind_t` table + ABI rules.
- The full security-reviewer "data-leakage map" section is in the security review doc and is referenced from init-specific revision tickets — not consolidated here.
- The "things the synthesis got right that I'd protect" section is in the critic doc and used informally to guide S1 implementer prompts toward not over-correcting.

---

**End of synthesis. After the precondition fixes land in the master plan + init-09 design doc, S1 implementer dispatch is unblocked.**
