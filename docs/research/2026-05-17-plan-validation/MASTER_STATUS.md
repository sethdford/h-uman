---
title: Master Status
date: 2026-05-17
status: audit
---

# Master Plan-Validation Status

**Audit date:** 2026-05-17
**Plans audited:** 119 (all under `docs/plans/` including `audit-followups/` and `adr/`)
**Method:** 11 auditor agents in parallel, status-triage depth, three-axis rubric (implemented / proven / wired), evidence from grep + file:line citations
**Per-plan reports:** [`by-plan/`](by-plan/) (one file per plan, frontmatter-tagged)
**Rubric:** see [`STATUS_TEMPLATE.md`](STATUS_TEMPLATE.md)

---

## TL;DR

> **Mature middle, ambitious head, honest tail.**
>
> ~55% of plans (66/119) are SHIPPED; another ~3% (4) ship code that isn't wired into production paths. The memory v2, human-fidelity, cognition, and adversarial-review/RL programs are largely real and integrated. The May 2026 Init series and the May 16 audit followups are mostly NOT_STARTED — the team has documented an ambitious next quarter but very little of it has landed yet. CLAUDE.md's own honest-status flags (M3 frontier bridge, persona overlay wiring, vault encryption) are corroborated.

---

## Verdict distribution (n=119)

| Verdict | Count | % | Meaning |
|---|---:|---:|---|
| **SHIPPED** | 66 | 55.5% | All three axes substantially FULL |
| **PARTIAL** | 30 | 25.2% | Significant scaffolding but missing pieces |
| **NOT_STARTED** | 13 | 10.9% | No code matching plan claims (some intentionally deferred) |
| **SHIPPED_UNWIRED** | 4 | 3.4% | Code + tests exist but no production callers |
| **SUPERSEDED** | 4 | 3.4% | Replaced by a later plan that did land |
| **OBSOLETE** | 2 | 1.7% | Design-only memo / explicit "do not build" |

Net unfinished work (PARTIAL + NOT_STARTED + SHIPPED_UNWIRED) = **47 plans (39.5%)**.

---

## Top 10 plans needing the most attention

Sorted by combination of strategic importance and gap size. **All citations are to per-plan reports in `by-plan/`.**

| # | Plan | Verdict | Why it matters |
|---|---|---|---|
| 1 | [m3-frontier-model-bridge](by-plan/2026-05-10-m3-frontier-model-bridge.md) | PARTIAL | "Real technical challenge" per CLAUDE.md. `hu_m3_frontier_adapter_noop_infer` literally returns `HU_OK` with no inference — 11 call sites all hit the noop. Bridge B (MLX) unstarted. |
| 2 | [audit-followups/01-persona-overlay-wiring](by-plan/2026-05-16-audit-followups/01-persona-overlay-wiring.md) | NOT_STARTED | Persona overlays unwired across all 43 channels — the M1 "Persona-First" thesis is materially incomplete at the channel layer. |
| 3 | [audit-followups/02-vault-encryption-migration](by-plan/2026-05-16-audit-followups/02-vault-encryption-migration.md) | NOT_STARTED | `src/security/vault.c` self-describes as XOR/base64 — secrets are obfuscated, not encrypted. |
| 4 | [audit-followups/03-hook-pipeline-invocation](by-plan/2026-05-16-audit-followups/03-hook-pipeline-invocation.md) | NOT_STARTED | Hook calls scattered across 4+ sites; `hu_agent_dispatch_tool` centralization not done. Risk: missed hook invocation. |
| 5 | [init-04-mlx-qwen3-provider](by-plan/2026-05-11-init-04-mlx-qwen3-provider.md) | PARTIAL | Apple-Silicon hero path; M3 Bridge B precondition. Currently a stub returning `HU_ERR_NOT_SUPPORTED`. |
| 6 | [memory-scoping-followups](by-plan/2026-05-15-memory-scoping-followups.md) | PARTIAL | FU-1 (contact_send_recency daemon wiring) not done — module exists at `src/context/contact_send_recency.c` with zero daemon callers. |
| 7 | [init-13-kv-compression](by-plan/2026-05-11-init-13-kv-compression.md) | SHIPPED_UNWIRED | DeltaKV + SWAN backends + 203-LOC test exist; **zero production callers** of `hu_kv_compressor_*`. Orphan code. |
| 8 | [project-scalpel](by-plan/2026-03-07-project-scalpel.md) / [-design](by-plan/2026-03-07-project-scalpel-design.md) | PARTIAL | Chat-view decomposition stalled at 1,289 LOC (target ~200); composer + message-list never extracted. |
| 9 | [w15-crypto-privacy](by-plan/2026-05-10-w15-crypto-privacy.md) | PARTIAL | DP-SGD + GDPR Article 20 export pending. Privacy thesis depends on this. |
| 10 | [adr/2026-05-11-public-benchmark-license](by-plan/adr/2026-05-11-public-benchmark-license.md) | NOT_STARTED | ADR specifies Apache-2.0; actual `LICENSE` file is **MIT**. Unflagged deviation — reconcile or amend ADR. |

---

## Plans shipping code that isn't wired (`SHIPPED_UNWIRED`)

These are the costliest kind of incompleteness — the work is done but the value isn't being captured:

| Plan | What's orphan |
|---|---|
| [chaos-tests-design](by-plan/2026-03-07-channel-conversation-chaos-tests-design.md) / [-plan](by-plan/2026-03-07-channel-conversation-chaos-tests-plan.md) | `human_channel_tests` target gated behind `-DHU_ENABLE_CHANNEL_TESTS=ON`; not in default CI |
| [synthetic-pressure-tests](by-plan/2026-03-07-synthetic-pressure-tests-design.md) | `human_synthetic` target gated behind `-DHU_ENABLE_SYNTHETIC=ON`; not in default CI |
| [init-13-kv-compression](by-plan/2026-05-11-init-13-kv-compression.md) | Two backends + tests, zero production callers |

The Tier-1 channel ambition (CLAUDE.md M6) hinges on running the first two of these regularly. Currently they only run if a human explicitly turns them on.

---

## Cross-cutting findings

### A. Strategic missions — reality check vs CLAUDE.md

| Mission | CLAUDE.md says | Audit says |
|---|---|---|
| **M1** Persona-first | "Done (Phase 1)" | **Confirmed at agent layer.** Persona unconditional in `hu_agent_t`; `hu_starter_persona_json` exists; onboarding wired. **But** Phase 2+ persona-overlay-per-channel is NOT_STARTED — overlays are defined but not rendered in any of 43 channel modules. |
| **M2** Personal model | "Hard. Single artifact, per-turn save, atomic-rename pinned" | **Confirmed.** `hu_personal_model_t` ingests/saves/builds prompts; atomic save pinned by `tests/test_personal_model_atomic_save.c`. Fact extraction still heuristic (plan honestly admits this). |
| **M3** Private learning | "Hardest. Bridge A daemon-pattern proven" | **Half true.** Daemon fallthrough safety pinned by `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` — but that test proves *safety*, not *learning*. `hu_m3_frontier_adapter_noop_infer` is a literal noop. Bridge B (MLX) unstarted. |
| **M4** Ship to users | "Medium. `human onboard` exists" | **Confirmed.** `src/onboard.c` (499 LOC) is real; first-run path wired at `src/main.c:952,2883`. |
| **M5** HuLa as platform | "Hard. SDK 0.1.0; tightly coupled" | **Confirmed.** SDK header at `include/human/hula_sdk.h`, but **`hu_hula_sdk_call/sequence/run_json` helpers have zero callers in tests/examples**. Internal DSL is real; platform is pre-alpha. |
| **M6** Channel focus | "Easy strategy, medium execution" | **Mostly editorial.** No runtime gating by tier; 43 channel `.c` files vs CLAUDE.md's claimed 31. Tier-1 vtables (`react`, `start_typing`, `load_conversation_history`) verified present for Telegram/Discord/iMessage/Slack. |

### B. Memory v2 — the gold-standard subsystem

19 of 24 W-series and roadmap plans SHIPPED with high confidence. The W7 facade keystone is real: `hu_memory_facade_t` declared in `include/human/memory/memory.h`; `agent_turn.c` routes through `agent->w7_facade` and `hu_w7_facade_memory_handle()`. Phase-1 bypass inventory is genuinely zero. The honest gaps (W10 KV-replay deferred via ADR, W13 frontier bridge open, W15 DP-SGD pending) are flagged by the team itself in plan frontmatter — i.e., the system reports its own status accurately. **This is the only large subsystem where audit findings substantially match team self-reports.**

### C. RL loop — closed end-to-end, not running nightly

Phases 0–6 all shipped (tag `rl-sota-phase-6-complete`). Reaction wiring is live: iMessage tapback + Slack reactji → `src/channels/reaction_event.c` → `src/agent/reaction_handler.c` → `dpo_pairs` SQLite. The single wiring gap is automation: **no dedicated `rl_nightly.yml` workflow** — validation runs via `scripts/validate-rl-sota.sh` on demand. The loop is closed; it just isn't on a scheduled trainer. This is a low-effort fix to materially unlock the self-improvement claim.

### D. May 16 audit followups — only the two highest-priority items landed

Of the 5 queued post-audit plans:
- **Landed in the audit session itself:** `HU_PERM_DENY` for unknown-tool masquerading; `hu_shell_must_deny_unsandboxed` predicate with 7 tests in `tests/test_shell_sandbox.c`.
- **NOT_STARTED:** persona overlay wiring (01), vault encryption (02), hook centralization (03), provider dispatch cleanup (05).
- **PARTIAL:** daemon decomposition (04) — 4 sibling modules extracted but `daemon.c` grew to 12,491 LOC.

These represent the most concrete near-term gap surface — they have written specs, named modules, and explicit acceptance criteria.

### E. Plan frontmatter is systematically stale

A recurring theme across groups 5, 7, 8: plans marked `status: proposed` or `status: in-progress` whose claims are fully landed in code. Examples:
- 2026-03-21 cognition plans (`dual-process`, `dynamic-skill-routing`, `elastic-memory-episodic`, `emotional-cognition`, `metacognitive-loop`) all `status: proposed` but fully wired across `agent_turn.c` + `agent_stream.c`.
- 2026-03-21 metacognitive-loop has 76 test references but says `in-progress`.
- Multiple W-series plans (W1, W2, W3) say `proposed` but reality is fully landed.

**Action:** sweep plan frontmatter to reflect reality. This is a 30-minute editing pass that would materially improve the team's signal-to-noise. The opposite drift (`status: implemented` but code missing) occurs on **digital-twin-master-plan** — calibration is CLI-invoked, not the closed measure-and-adapt loop the plan describes.

### F. Plan-vs-code path drift

The 2026-03-21 cognition plans placed modules under `src/agent/` (e.g., `src/agent/metacognition.c`); the implementation consolidated them under `src/cognition/`. Functionally harmless but breaks any "find by path" navigation from plan to code. **Action:** plans cite implementation paths as a contract; if it moved, the plan should move with it.

### G. Tests that pin claims vs tests that exist

Several groups noted weak test coverage on shipped UI primitives (Group 1: hu-data-table-v2 interactions, dynamic-light cursor mapping) and shipped channel infra (Group 2: per-channel naturalness eval suites). The in-repo rule `.claude/rules/tests-that-pin-bugs.md` warns about exactly this pattern. **Recommendation:** add a per-component contract-test requirement to the design-system DoD.

---

## Per-group summaries

### Group 1 — UI/Design (12 plans)

5 SHIPPED, 3 PARTIAL, 4 SUPERSEDED. SOTA Design Overhaul + UX Sweep (4 waves, 13 primitives) shipped cleanly; Project Prism (Deep Steel direction) was abandoned for palette-expansion's teal+amber+indigo and is correctly marked superseded. The standout gap is **Project Scalpel**: chat-view decomposition stalled at 1,289 LOC vs ~200 target — `hu-composer` and `hu-message-list` were never extracted; the team adopted `hu-message-thread` instead without updating the plan.

### Group 2 — Channels & Communications (7 plans)

3 SHIPPED, 3 SHIPPED_UNWIRED, 1 OBSOLETE. Tier-1 channel **infrastructure** is shipped; Tier-1 channel **evidence** (naturalness eval, per-channel timing) is dormant because the harnesses (`human_channel_tests`, `human_synthetic`) are opt-in CMake targets not in default CI. iMessage Tier B feasibility correctly recorded as a "no" — none of its proposed APIs landed.

### Group 3 — Better-than-Human / Gateway / Competitive (6 plans)

5 SHIPPED, 1 PARTIAL. The 1,470-line `better-than-human.md` mega-plan's 7 layers (STM, commitments, pattern radar, circadian persona, LLMCompiler, superhuman services, tool router) all wired in `agent_turn.c`. Only caveat: **LLMCompiler defaults dormant** (`llm_compiler_enabled` flag + `tc_count >= 3` heuristic). Currently runs only when explicitly enabled.

### Group 4 — Human Fidelity (12 plans)

11 SHIPPED, 1 PARTIAL. **CLAUDE.md's M1 Phase 1 "done" claim verifies in detail** — no `HU_NO_PERSONA` guards remain; tapback `message_id` wiring at `src/channels/imessage.c:3920`; fillers/quirks applied in `src/daemon.c:10781,10807`. The PARTIAL is **missing-seven** — *contact knowledge state* (#3, what each contact has been told) and *shared compression / IYKYK shorthand* (#7) have no dedicated modules. These are the uncanny-valley risks the plan named, and they're real.

### Group 5 — Cognition & Digital Twin (10 plans)

6 SHIPPED, 3 PARTIAL, 1 OBSOLETE. **The cognition stack is more shipped than the plan metadata suggests** (all five 2026-03-21 plans say `proposed` but are fully wired). Weakest link: **self-improvement closure** — outcome-attribution → skill-trust update edge described in `evolving-cognition.md` doesn't exist by that name; opinion/preference extraction landed instead. **Digital-twin-master-plan is the most overclaimed** by its own frontmatter (`status: implemented` but calibration loop is CLI-only).

### Group 6 — HuLa & Platform & Strategic (3 plans)

2 SHIPPED, 1 PARTIAL. Strategic-missions plan is **unusually honest** — self-marks Q2 items as DONE and Q3+ open; every "DONE" claim spot-checked has corresponding code. HuLa core IR (2,329 LOC), compiler (676 LOC), emergence (476 LOC), 74 tests are production. The **public SDK helpers have zero callers** outside the executor — HuLa-as-internal-DSL is shipped; HuLa-as-platform is pre-alpha.

### Group 7 — Memory v2 + W-series (24 plans)

19 SHIPPED, 5 PARTIAL, 0 NOT_STARTED. **Most mature initiative audited.** W1-W6 uniformly shipped with deep agent_turn.c + daemon.c wiring. W7 facade keystone real and bypass-free. Honest gaps where the team flags them (W10 KV-replay deferred via ADR, W13 frontier bridge open, W15 DP-SGD pending).

### Group 8 — Behavior v1 / D2 / M3 / SOTA (6 plans)

2 SHIPPED, 3 PARTIAL, 1 NOT_STARTED (intentional defer). **M3 frontier bridge is the most-cited bottleneck** — the 11 `hu_agent_m3_on_provider_success` call sites all route to a literal noop. SOTA roadmap month 1 deliverables in place (A1 training data, A4 personal model upgrades, C1 onboarding); load-bearing **A3 milestone (E4B draft adapter)** not started — and the plan itself calls A3 "the single point where personalization and performance meet."

### Group 9 — Adversarial Review + RL Loop (10 plans)

8 SHIPPED, 2 PARTIAL. All 4 May-11 BLOCKERs closed; S-MAJOR MINJA broadening landed. RL loop phases 0–6 all shipped through tag `rl-sota-phase-6-complete`. **One gap stands out: no scheduled nightly trainer** — `scripts/validate-rl-sota.sh` runs on demand only.

### Group 10 — Init Series 01–14 (14 plans)

1 SHIPPED, 1 SHIPPED_UNWIRED, 7 PARTIAL, 5 NOT_STARTED. **The May 2026 Init program is overwhelmingly a roadmap document, not shipped code.** Only Init 09 (memory trust tiers) is unambiguously SHIPPED. **Six of the deepest ML/training initiatives (01 activation-steering, 02 molora, 05 verifier-driven-ttt, 06 simpo/orpo/grpo2, 08 federated-lora) have zero implementation hits beyond the plan documents.** Roughly 7% of the Init program is real and integrated.

### Group 11 — Audit Followups + ADRs + Rename Reports (15 plans)

4 SHIPPED, 4 PARTIAL, 7 NOT_STARTED. **W0a (episode) and W0b (MCP) renames both fully clean** — name slots free for downstream init plans. Memory-scoping shipped items confirmed but FU-1 (contact_send_recency daemon wiring) deferred. **License ADR mismatch:** ADR says Apache-2.0; LICENSE file is MIT — unflagged deviation.

---

## Recommended next moves (in priority order)

1. **Wire persona overlays into channel render path.** This is the M1 Phase 2 work; until it lands, "persona-first" stops at the agent boundary. — [audit-followups/01](by-plan/2026-05-16-audit-followups/01-persona-overlay-wiring.md)
2. **Resolve M3 Bridge B (MLX) or document the deferral as an ADR.** Currently the "personalization" pitch leans on infra that returns `HU_OK` with no inference. Either ship the bridge or be explicit in CLAUDE.md that M3 is unshipped. — [m3-frontier-model-bridge](by-plan/2026-05-10-m3-frontier-model-bridge.md), [init-04](by-plan/2026-05-11-init-04-mlx-qwen3-provider.md)
3. **Replace vault.c XOR with real crypto.** Calling it "secret storage" with XOR/base64 is a credibility risk. — [audit-followups/02](by-plan/2026-05-16-audit-followups/02-vault-encryption-migration.md)
4. **Centralize hook invocation.** 4+ scattered sites is a recipe for missed audits. The predicate-extraction rule (`security-predicate-extraction.md`) is the template. — [audit-followups/03](by-plan/2026-05-16-audit-followups/03-hook-pipeline-invocation.md)
5. **Enable channel-test + synthetic-pressure CMake targets in default CI.** The harnesses exist; just stop gating them. Closes the Tier-1 evidence gap for M6. — [chaos-tests-plan](by-plan/2026-03-07-channel-conversation-chaos-tests-plan.md), [synthetic-pressure](by-plan/2026-03-07-synthetic-pressure-tests-design.md)
6. **Wire init-13 KV compression into the llamacpp provider** OR delete the code. SHIPPED_UNWIRED is the worst state — implementation cost paid, value uncaptured. — [init-13](by-plan/2026-05-11-init-13-kv-compression.md)
7. **Wire `contact_send_recency` into the daemon.** Module exists; needs callers. — [memory-scoping-followups](by-plan/2026-05-15-memory-scoping-followups.md)
8. **Sweep plan frontmatter.** 30-minute editorial pass; especially the 2026-03-21 cognition plans, W-series, and digital-twin-master-plan.
9. **Schedule the RL trainer nightly.** `scripts/validate-rl-sota.sh` exists; add `.github/workflows/rl-nightly.yml`. Closes the self-improvement loop's last automation gap.
10. **Reconcile the LICENSE/ADR mismatch.** Either amend ADR or replace LICENSE. — [adr/public-benchmark-license](by-plan/adr/2026-05-11-public-benchmark-license.md)

---

## Methodology notes & caveats

- **Status-triage depth.** Audits relied on grep + targeted file reads, not on running the code. A plan classified SHIPPED has *evidence of code, tests, and wiring* — not necessarily *evidence that behavior is correct under load*. Where deeper verification matters (M3, RL loop, persona overlay), follow up with `/verify` against the named contracts.
- **Confidence is per-plan.** See individual report files for HIGH/MEDIUM/LOW confidence. Medium-confidence verdicts (e.g., `sota-quiet-mastery-design`, `daemon-decomposition`) deserve deeper inspection.
- **The audit didn't reread the plans for accuracy of *intent*.** Some plans may have been intentionally abandoned (e.g., Prism's Deep Steel direction). Those show as SUPERSEDED.
- **One known correctness gap.** Group 11 flagged `public-benchmark-license.md` ADR/LICENSE mismatch — Apache-2.0 ADR vs MIT actual LICENSE. Verifying this is a 1-line `head LICENSE` check.

---

## Files written by this audit

```
docs/research/2026-05-17-plan-validation/
├── MASTER_STATUS.md            (this file)
├── STATUS_TEMPLATE.md          (rubric)
└── by-plan/                    (119 status files, one per plan)
    ├── 2026-03-03-*.md         (5 files)
    ├── 2026-03-07-*.md         (11 files)
    ├── 2026-03-08-*.md         (2 files)
    ├── 2026-03-09-*.md         (2 files)
    ├── 2026-03-10-*.md         (13 files)
    ├── 2026-03-15-*.md         (1 file)
    ├── 2026-03-19-*.md         (1 file)
    ├── 2026-03-20-*.md         (2 files)
    ├── 2026-03-21-*.md         (6 files)
    ├── 2026-03-22-*.md         (3 files)
    ├── 2026-04-11-*.md         (2 files)
    ├── 2026-05-10-*.md         (30 files: W-series + roadmaps)
    ├── 2026-05-11-*.md         (29 files: init + RL + adversarial)
    ├── 2026-05-15-*.md         (1 file)
    ├── 2026-05-16-audit-followups/ (6 files)
    ├── adr/                    (6 files)
    └── AUDIT-REPORT-2026-03-11.md
```

Total: **121 markdown files** written (119 plan reports + master + template).
