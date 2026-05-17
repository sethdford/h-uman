# Sprint 7 Plan — Digital Twin via Gemma DPO + Continuous Personalization

**Date:** 2026-05-16
**Scrum Master:** claude-sonnet-4-6

## Branch
`sprint-7-digital-twin-dpo`

## Working directory
`/Users/sethford/Projects/h-uman/.claude/worktrees/hardcore-goldwasser-af5a11`

## Sprint base SHA
`13b89763`

## Sprint metadata
- **Stories:** 10 (US-7.1 through US-7.10)
- **Estimates:** 2×S + 5×M + 2×L + 1×S (net: ~6 M-equivalents; roughly 3–4 engineering days of parallel agent work)
- **Design docs:** `sprints/sprint-7/designs/US-7.X.md` (one per story, ~2120 total lines)
- **Decision log:** `sprints/sprint-7/decisions.md` (D1–D4; binding AC revisions for US-7.1, 7.2, 7.3, 7.6)

---

## §1 Sequencing

```
Wave 0 (parallel, no deps):
  US-7.1  DPO preference pass               [P0, MEDIUM, M]
  US-7.2  Mine DPO pairs from corrections   [P0, MEDIUM, M]
  US-7.3  Honesty gate (INS-B)              [P0, LOW,    S]
  US-7.6  Judgment-fidelity eval (INS-A)    [P1, MEDIUM, M]

Wave 1 (after ALL Wave 0 commits verified on sprint branch):
  US-7.4  Rank + target-modules expansion   [P1, LOW,    S]  ← US-7.1
  US-7.5  W14 nightly re-train cron         [P1, MEDIUM, M]  ← US-7.1, US-7.2
  US-7.7  Best-of-N at inference            [P1, MEDIUM, M]  ← US-7.3
  US-7.9  Constitutional style self-critique[P2, MEDIUM, M]  ← US-7.3

Wave 2 (after ALL Wave 1 commits verified AND check-lora-ab.sh + check-lora-baseline.sh green):
  US-7.8  MoLoRA static router              [P2, MEDIUM, L]  ← US-7.1, US-7.2, US-7.5
  US-7.10 ORPO/SimPO vtable pilot           [P2, MEDIUM, L]  ← US-7.1
```

---

## §2 Wave Assignments Table

### Wave 0

| Story | Title | Risk | Key files touched | Implementer agent | Verifier scope | Critic scope | Aspect-panel? | Worktree isolation? |
|---|---|---|---|---|---|---|---|---|
| US-7.1 | DPO preference pass | MEDIUM | `scripts/finetune-gemma.py` (rewrite `run_dpo()`), `tests/test_finetune_gemma_dpo.py` (ADD), `tests/fixtures/dpo_pairs_min.jsonl` (ADD), `tests/fixtures/dpo_pairs_min.db` (ADD), `requirements.txt` (pin mlx-lm-lora) | `general-purpose` | AC-7.1.1–7.1.5; `test_finetune_gemma_dpo.py` green; `check-lora-ab.sh` exits 0; `check-lora-baseline.sh` exits 0 | Correctness of DPO loss path, mlx-lm-lora pin, no SFT-under-DPO regression | **YES** (DPO training pipeline correctness) | YES |
| US-7.2 | Mine DPO pairs from corrections | MEDIUM | `include/human/ml/dpo_miner.h` (ADD), `src/ml/dpo_miner.c` (ADD), `src/ml/cli.c` (+`hu_ml_cli_mine_corrections`), `src/main.c` (+subcommand), `CMakeLists.txt`, `tests/test_dpo_miner.c` (ADD), `tests/test_main.c` | `general-purpose` | AC-7.2.1–7.2.6; `test_dpo_miner.c` green; zero ASan; `-Wall -Wextra -Wpedantic -Werror` clean | PII redaction coverage, dedup correctness, schema alignment with `hu_dpo_init_tables` | **YES** (PII + security on preference pair storage) | YES |
| US-7.3 | Honesty gate (INS-B) | LOW | `src/daemon.c` (one-shot warn + D4 static guard), `src/doctor.c` (or equivalent doctor extension point), `tests/test_provider_all.c` (extend), `tests/test_doctor_personalization_warning.c` (ADD) | `general-purpose` | AC-7.3.1–7.3.5; existing `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` unchanged | Log emission, no-spurious-warning cases, `HU_IS_TEST` reset shim per D4 | No (LOW risk; critic-only) | YES |
| US-7.6 | Judgment-fidelity eval (INS-A) | MEDIUM | `include/human/ml/fidelity.h` (+`hu_ml_fidelity_score_judgment`, `hu_ml_nll_compute_fn_t`), `src/ml/fidelity.c`, `src/ml/cli.c` (extend `hu_ml_cli_fidelity_status`), `scripts/check-lora-ab.sh` (+`--judgment` flag + SKIP output), `tests/test_ml_fidelity_judgment.c` (ADD), `tests/fixtures/judgment_fidelity_holdout.jsonl` (ADD, ≥10 rows), `tests/test_check_lora_ab_judgment.sh` (ADD) | `general-purpose` | AC-7.6.1–7.6.5; `check-lora-ab.sh --judgment` emits parseable SKIP; `hu_communication_style_fidelity_score` signature unchanged | D3 contract: SKIP must be non-pass-interpretable; no real weights loaded; backward compat on existing callers | **YES** (gate-script correctness + seam-dormant contract) | YES |

### Wave 1

| Story | Title | Risk | Key files touched | Implementer agent | Verifier scope | Critic scope | Aspect-panel? | Worktree isolation? |
|---|---|---|---|---|---|---|---|---|
| US-7.4 | Rank + target-modules expansion | LOW | `scripts/finetune-gemma.py` (+`--target-modules`, `--rank` defaults, `train_config.json` writer), `tests/test_finetune_gemma_modules.py` (ADD) | `general-purpose` | AC-7.4.1–7.4.5; `check-lora-baseline.sh` exits 0 | Default rank-32 is sane, `train_config.json` schema is additive | No (LOW risk; critic-only) | YES |
| US-7.5 | W14 nightly re-train cron | MEDIUM | `src/ml/lora_retrain_runner.c` (ADD), `include/human/ml/lora_retrain_runner.h` (ADD), `src/daemon.c` (enqueue site), `~/.human/scheduler.status` schema (lora_retrain block), `tests/test_w14_lora_retrain.c` (ADD), `tests/test_scheduler_status.c` (extend), `tests/fixtures/candidate_adapter_metadata.json` (ADD) | `general-purpose` | AC-7.5.1–7.5.5; no current adapter modified on failure; `lora_retrain_skipped_no_new_data` fires when 0 new pairs | Symlink-swap atomicity, promotion gate semantics, `HU_IS_TEST` subprocess guard, no tick starvation (synchronous V1 per open-Q 1 default) | **YES** (adapter promotion path; continuous-learning loop integrity) | YES |
| US-7.7 | Best-of-N at inference | MEDIUM | `src/providers/llamacpp.c` (+best-of-N wrapper), `include/human/providers/llamacpp.h` (new config keys), `src/agent_turn.c` (config plumbing), `tests/test_llamacpp_best_of_n.c` (ADD), `tests/test_doctor_best_of_n_warning.c` (ADD) | `general-purpose` | AC-7.7.1–7.7.6; best_of_1 is single-call; cloud provider warning fires; `hu_communication_style_fidelity_score` signature unchanged | Inference hot-path gating (disabled by default), telemetry event schema, cost-cap semantics | **YES** (inference hot path + regression risk) | YES |
| US-7.9 | Constitutional style self-critique | MEDIUM | `src/persona/style_critique.c` (ADD), `include/human/persona/style_critique.h` (ADD), `src/agent_turn.c` (post-generation hook), `tests/test_style_self_critique.c` (ADD), `tests/test_style_critique_patterns.c` (ADD) | `general-purpose` | AC-7.9.1–7.9.5; disabled-by-default guard; max-one-regen cap; ≥5 pattern tests | Regen-loop prevention (identical-output risk), false-positive substring analysis, disabled-path has zero call | **YES** (agent response path mutation) | YES |

### Wave 2

| Story | Title | Risk | Key files touched | Implementer agent | Verifier scope | Critic scope | Aspect-panel? | Worktree isolation? |
|---|---|---|---|---|---|---|---|---|
| US-7.8 | MoLoRA static router | MEDIUM | `include/human/ml/molora.h` (ADD), `src/ml/molora_router.c` (ADD), `src/providers/llamacpp.c` (router call-site), config parser (new `molora.*` keys), `CMakeLists.txt`, `tests/test_molora_router.c` (ADD); `cmake --preset release` binary-size delta check | `general-purpose` | AC-7.8.1–7.8.5; `HU_ENABLE_MOLORA=OFF` → zero behavior change; binary delta ≤ 8 KB | Composition rule with US-7.7 (adapter-first, then sample-N); fallback-to-default correctness; feature-flag guard integrity | **YES** (conditional compile + inference dispatch) | YES |
| US-7.10 | ORPO/SimPO vtable pilot | MEDIUM | `include/human/ml/rl_trainer.h` (ADD), `src/ml/rl_trainer.c` (ADD, SimPO factory only), `src/ml/cli.c` (+`hu_ml_cli_rl_train`), `src/main.c` (+`rl-train` subcommand / alias wiring), `tests/test_rl_trainer_simpo.c` (ADD), `tests/test_ml_cli_rl_train.c` (ADD) | `general-purpose` | AC-7.10.1–7.10.6; SimPO golden loss within 1e-4; ORPO/GRPO-2 stub exits 2; DPO backward compat unchanged; zero ASan | vtable deinit idempotency, DPO alias plumbing, Init #06 divergence note filed as docs debt | **YES** (new vtable + ML loss correctness) | YES |

---

## §3 Per-Story Implementer Prompt Template

The following template is instantiated per story. Replace `{{US_ID}}`, `{{TITLE}}`, `{{DESIGN_DOC}}`, `{{AC_BLOCK}}`, and `{{FILES_BLOCK}}` for each dispatch.

---

```
You are a general-purpose implementer agent for sprint-7 of the h-uman project.

## Sprint context
- Sprint branch: sprint-7-digital-twin-dpo
- Sprint base SHA: 13b89763 (all your commits go on top of this)
- Working directory: /Users/sethford/Projects/h-uman/.claude/worktrees/hardcore-goldwasser-af5a11
- Decision log (BINDING — read before writing a line): sprints/sprint-7/decisions.md
  D1 revises US-7.1 AC-7.1.1 (mlx-lm-lora flag spelling)
  D2 revises US-7.2 AC-7.2.1 and AC-7.2.4 (chat.db signal; hu_pii_redact not hu_personal_model_redact_pii)
  D3 governs US-7.6 (seam ships dormant; SKIP must be non-pass)
  D4 governs US-7.3 (one-shot static warn; HU_IS_TEST reset shim required)

## Your assignment
Story: {{US_ID}} — {{TITLE}}
Design doc: sprints/sprint-7/designs/{{DESIGN_DOC}}
Read the ENTIRE design doc before writing code. Section §0 (critical mismatches) and §7 (open questions) are mandatory reads.

## Acceptance criteria (verbatim from stories.md, subject to decisions.md revisions above)
{{AC_BLOCK}}

## Files you are expected to touch
{{FILES_BLOCK}}
(All other files are out of scope unless the design doc explicitly says otherwise.)

## Rules (non-negotiable)
1. C code: C11, -Wall -Wextra -Wpedantic -Werror, zero ASan errors under cmake --preset dev.
2. Never use SQLITE_TRANSIENT — use SQLITE_STATIC (null).
3. All tests: deterministic, no real network, no process spawning outside HU_IS_TEST guards.
4. Never reference Gemini 2.0 or 2.5 models. Not applicable to this sprint, but noted.
5. KISS/YAGNI: implement exactly what the AC requires; no speculative abstractions.
6. One concern per commit. Do not mix feature + refactor + test in one commit.

## Build and test commands
cmake --preset dev && cmake --build --preset dev
./build/human_tests --suite=<relevant suite>   # targeted during development
./build/human_tests                             # full suite — must be 0 failures before commit

## MANDATORY before reporting DONE
Before you report DONE you MUST:
1. Run the full test suite: ./build/human_tests — must show 0 failures, 0 ASan errors.
2. Run scripts/check-lora-ab.sh and scripts/check-lora-baseline.sh if this story touches the fine-tune pipeline (US-7.1, US-7.4, US-7.5, US-7.6).
3. Stage and commit ALL your work to the sprint branch:
   git add <specific files — list them explicitly; never git add .>
   git commit -m "feat({{scope}}): {{description}}"
   The commit MUST land on sprint-7-digital-twin-dpo (not a detached HEAD, not a sub-branch).
4. Verify the commit appears in:
   git log sprint-7-digital-twin-dpo ^13b89763 --oneline
5. THEN and only then report DONE by outputting:
   IMPLEMENTER_DONE: {{US_ID}} sha=<your commit sha>

Working-tree-only DONE reports will be rejected. A concurrent agent's git reset can wipe uncommitted work between your report and the verifier run.
```

---

## §4 Quality Gate Sequence Per Story

This loop runs for every story before it can be marked closed.

```
Step 1 — Implementer commit
  git log sprint-7-digital-twin-dpo ^13b89763 --oneline | grep <story pattern>
  If not present: story is NOT done. Re-dispatch implementer.

Step 2 — /verify (spawn verifier agent)
  Input: the commit sha from Step 1, the AC block, the test commands from the design doc.
  Pass condition: RESULT_verifier=PASS
  Fail condition: RESULT_verifier=FAIL or INCONCLUSIVE → story re-opens; do NOT advance.

Step 3 — /critic (per-story, adversarial; NOT batched at sprint end)
  Input: the diff of Step 1's commit against 13b89763 (git diff 13b89763..<sha>).
  Pass condition: RESULT_critic=CLEAN or LOW/INFO only findings.
  Fail condition: RESULT_critic=HAS_FINDINGS with HIGH or CRITICAL severity → story re-opens;
                  findings become new tasks tagged CRITIC-<US_ID>-<N>; implementer re-dispatched.

Step 4 — /aspect-panel (only for stories marked "YES" in §2 table)
  Input: same diff as Step 3. Panel covers: correctness, edge-case, security, regression, style.
  Pass condition: RESULT_aspect-panel=PASS or CLEAN.
  ESCALATE condition: surface to Seth before closing the story. Do not unilaterally advance.
  Stories that receive critic-only (US-7.3, US-7.4): skip this step.

Step 5 — Mark DONE
  All four conditions must hold:
  [ ] Commit exists in git log sprint-7-digital-twin-dpo ^13b89763
  [ ] RESULT_verifier=PASS
  [ ] RESULT_critic=CLEAN (or LOW/INFO only)
  [ ] RESULT_aspect-panel=PASS or CLEAN (if panel required), or panel step was legitimately skipped
```

---

## §5 Cross-Story Coordination Notes

1. **`~/.human/scheduler.status` lora_retrain block (US-7.5 + future sprints).** US-7.5 adds a `lora_retrain` top-level key to the scheduler status JSON. Any future story touching `scheduler.status` must not stomp this key. The schema is: `{"last_run_ts": <int>, "last_outcome": "skipped|pass|fail", "pairs_consumed": <int>}`. US-7.5's implementer must extend `tests/test_scheduler_status.c` to assert the parser round-trips this block.

2. **Inference dispatch composition: US-7.7 (best-of-N) + US-7.8 (MoLoRA) ordering.** Both stories touch `src/providers/llamacpp.c`'s inference path. The composition rule (from the US-7.8 design) is: **adapter selection (MoLoRA router, US-7.8) fires first; then the chosen completion path is sampled N times (US-7.7 best-of-N).** US-7.7 lands in Wave 1; US-7.8 lands in Wave 2. The US-7.8 implementer must read US-7.7's committed diff before touching `llamacpp.c` to avoid overwriting the best-of-N call sites. Scrum master will verify no silent regression on AC-7.7.1 after US-7.8 commits.

3. **US-7.10 vtable divergence from Init #06.** The three-member vtable (`train_step`, `compute_loss`, `deinit`) in the AC is canonical for this sprint. After US-7.10 merges, a docs-only follow-up (not blocking Sprint 7 close) must update `docs/plans/2026-05-11-init-06-simpo-orpo-grpo2.md` to document the v1 surface and the planned widening path (add `name`, `prepare`, `save_adapter` before ORPO/GRPO-2 land). Sprint auditor will flag this if it is not filed.

4. **US-7.2 miner output location must align with US-7.1 `--from-corrections` search path.** The US-7.2 design (open question Q3) recommends the miner export a JSONL to `~/.human/dpo/pairs.jsonl` so `finetune-gemma.py --from-corrections` can locate it without opening `memory.db` from Python. US-7.1 and US-7.2 implementers MUST coordinate on this path before either commits. If they work in separate worktrees (they will), the scrum master will confirm the agreed path is identical before advancing either to verifier.

5. **Wave 2 gate.** Wave 2 MUST NOT start until `scripts/check-lora-ab.sh` AND `scripts/check-lora-baseline.sh` both exit 0 on the sprint branch. This is the "Wave 1 is stable" signal called out in stories.md.

---

## §6 Budget and Timing Estimate

| Phase | Agent-runs | Estimated cost |
|---|---|---|
| Wave 0: 4 implementers (parallel) | 4 | ~$15–25 |
| Wave 1: 4 implementers (parallel) | 4 | ~$15–25 |
| Wave 2: 2 implementers (parallel) | 2 | ~$6–12 |
| Verifier × 10 stories | 10 | ~$15 |
| Per-story critic × 10 stories | 10 | ~$12 |
| Aspect-panel × 8 stories (US-7.3 and US-7.4 skip) | 8 | ~$20 |
| Sprint review + sprint-auditor adversarial audit | 2 | ~$4–6 |
| Retro + tag-sprint-close.sh | — | ~$1 |
| **Total** | | **~$88–116** |

Timing estimate: Wave 0 ~45–75 min (parallel), Wave 1 ~60–90 min (parallel), Wave 2 ~90–120 min (parallel). Wall clock ~3.5–5 hours including gate checks between waves.

---

## §7 Open Questions for Seth

Questions are grouped by story. Each is marked **NON-BLOCKING** (proceed with design default) or **BLOCKING** (implementer must not start until resolved).

### US-7.1 — DPO preference pass

**Q1.1 — AC-7.1.1 literal string assertion** (NON-BLOCKING — D1 resolves this)
D1 already amends the AC: the test asserts `-m mlx_lm_lora.train --train-mode dpo --train-type lora` in argv, not `--fine-tune-type dpo`. Implementer follows D1. No action needed.

**Q1.2 — AC-7.1.2 delta semantics: delta-of-deltas vs absolute gate** (NON-BLOCKING)
Design default: satisfy the existing absolute gate in `check-lora-ab.sh` for this sprint; defer the delta-of-deltas gate to a follow-on US-7.6.1 story in Sprint 8 once judgment-PPL is wired. If you want the stricter gate now, estimate grows M→L and the story needs a re-plan before implementer starts.

**Q1.3 — Third-party `mlx-lm-lora` dependency** (NON-BLOCKING — D1 approves it)
D1 explicitly accepts the dependency with the pin `mlx-lm-lora>=2.1.0,<3` and requires a CI smoke test that the wheel installs. Implementer proceeds.

---

### US-7.2 — Mine DPO pairs from corrections

**Q2.1 — AC-7.2.1 literal interpretation unsatisfiable** (NON-BLOCKING — D2 resolves this)
D2 reinterprets the AC as mining the existing `chat.db` user-correction signal (3-turn pattern via `hu_training_data_extract_dpo`), not a non-existent `draft_text/sent_text` SQLite table. Implementer follows D2.

**Q2.2 — Contact-name PII in AC-7.2.4** (NON-BLOCKING — recommend option (a))
`hu_pii_redact` covers email/phone/SSN/CC/IPv4/secrets; it does NOT cover bare contact names. Recommendation: the test `miner_redacts_pii` asserts email + phone + SSN coverage only; contact-name NER is filed as a follow-on story. The AC says "contact name or email" but the implementation cannot satisfy contact-name redaction without an NER-grade feature that is explicitly out of scope. **Confirm: proceed with email/phone/SSN only?** If you want contact-name coverage, identity_links lookup adds ~80 LOC and a personal-model dependency — estimate grows S within M, still M-sized.

**Q2.3 — Miner output location for `--from-corrections` path** (NON-BLOCKING — recommend option (a))
Recommendation: miner exports to `~/.human/dpo/pairs.jsonl` as a known location; `finetune-gemma.py --from-corrections` reads from that path. Avoids adding an SQLite dependency to the Python script. US-7.1 and US-7.2 implementers must agree on this path before committing; scrum master will coordinate between worktrees.

---

### US-7.4 — Rank + target-modules expansion

**Q4.1 — Default target modules: QKVO or include MLP (gate/up/down)** (NON-BLOCKING)
Design default: rank 32, modules `q_proj,k_proj,v_proj,o_proj` (QKVO). The 7-module option is behind `--target-modules` flag. No change needed unless you want MLP included by default — that is a test-fixture update and nothing more.

---

### US-7.5 — W14 nightly re-train cron

**Q5.1 — Subprocess synchronous vs. fork-and-poll** (NON-BLOCKING — proceed with synchronous V1)
Design default: synchronous `waitpid` blocking the scheduler tick for the duration of training. This is correct for V1; tick starvation is addressed by raising `HU_SCHED_TOTAL_BUDGET_MS` for the LoRA-retrain tick specifically (not a global raise). Defer the async state-machine variant to a follow-up sprint if real wall-clock data shows tick starvation.

**Q5.2 — `human ml promote-adapter` subcommand vs inline C swap** (NON-BLOCKING)
Design default: dedicated `promote-adapter` subcommand for reusability. Proceed with the subcommand approach.

**Q5.3 — Nightly enqueue site in `daemon.c` vs `world_model_bridge.c`** (NON-BLOCKING)
Design default: enqueue adjacent to the existing `hu_w14_scheduler_enqueue_lora` site in `src/daemon.c` housekeeping path. Proceed with this unless you have a strong reason to relocate.

---

### US-7.6 — Judgment-fidelity eval (INS-A)

**Q6.1 — Ship seam dormant vs. wire `src/ml/gpt.c` now** (NON-BLOCKING — D3 resolves this)
D3 decides: ship dormant. `check-lora-ab.sh --judgment` emits visible SKIP (non-pass). Real GPT wiring is US-7.6.1 in Sprint 8.

**Q6.2 — `jq` in `check-lora-ab.sh`** (NON-BLOCKING — recommend `command -v jq` guard)
Design default: add `command -v jq || { echo "[judgment] SKIP jq not installed"; exit 0; }` at the top of the `--judgment` block. This keeps the gate script non-fragile in CI environments without jq while avoiding a hard dependency. Proceed with this approach.

**Q6.3 — Default delta floor `LORA_AB_JUDGMENT_PPL_DELTA_FLOOR=0.05`** (NON-BLOCKING)
Proceed with the 0.05 placeholder documented as a placeholder. The fixture's leading comment will say: "this floor is a placeholder; measure variance once the bridge lands before tightening."

---

### US-7.7 — Best-of-N at inference

No open questions from the design doc. The design is complete and self-consistent. One clarification note for the implementer: "called exactly N times" means the mock completion function's call count equals N — cost-cap exit counts partial N as "best so far" (AC-7.7.5 semantics; design §4 covers this explicitly).

---

### US-7.8 — MoLoRA static router

**Q8.1 — Flat map vs nested objects in `molora.channel_adapters`** (NON-BLOCKING)
Design default: flat map `{channel_id: adapter_path_string}`. Phase 2 (Sprint 8+) will break schema compat with a versioned bump if scale/weight fields are needed. Proceed with flat map.

**Q8.2 — `molora.enabled = true` when `personalization.enabled = false`** (NON-BLOCKING)
Design default: MoLoRA is gated on the parent `personalization.enabled` flag. If personalization is off, the MoLoRA router stays disabled regardless of `molora.enabled`. Mirror the existing persona overlay behavior.

**Q8.3 — `human doctor` warning for missing adapter path** (NON-BLOCKING)
Design default: file as a DEBT- follow-on task. Not blocking US-7.8 close.

---

### US-7.9 — Constitutional style self-critique

**Q9.1 — Should violating drafts feed DPO pair mining?** (NON-BLOCKING — out of scope)
Out of scope for US-7.9. File as follow-on story candidate. The style-critique decision is mechanical (rule-based), not preference-based, and may bias the DPO collector. A design spike is recommended before committing to this in a future sprint.

**Q9.2 — New `hu_observer_event_tag_t` enum value for `STYLE_RULE_VIOLATION_UNRESOLVED`?** (NON-BLOCKING)
Design default: `hu_log_info` + test counter for V1. Defer enum value until a downstream caller programmatically reacts to this event.

**Q9.3 — Alias table in code vs JSON config** (NON-BLOCKING)
Design default: hard-code for V1. Document the location. JSON-ify only if users explicitly ask.

---

### US-7.10 — ORPO/SimPO vtable pilot

**Q10.1 — `human ml rl-train --algorithm dpo` as alias vs distinct command** (NON-BLOCKING — recommend alias)
Design default: `rl-train --algorithm dpo` internally calls `hu_ml_cli_dpo_train` with rewritten argv. Old `dpo-train` subcommand stays registered. Implementer proceeds with alias approach (AC-7.10.4 satisfied cheaply).

**Q10.2 — Init #06 divergence: update plan doc after merge** (NON-BLOCKING)
After US-7.10 merges, update `docs/plans/2026-05-11-init-06-simpo-orpo-grpo2.md` to document the three-member v1 vtable surface. This is a docs-only follow-up, not a blocker.

**Q10.3 — Factory naming: `hu_rl_trainer_simpo_create` vs `hu_rl_trainer_create_simpo`** (NON-BLOCKING — AC wins)
Use `hu_rl_trainer_simpo_create` (AC form). Note in `src/ml/CLAUDE.md` that `<module>_<algo>_<action>` is a documented variant for vtable-family factories.

---

## Summary: BLOCKING open questions

**None.** All open questions are NON-BLOCKING with clear defaults from the design docs and the D1–D4 decision log. Wave 0 can be dispatched immediately.

---

*Sprint plan authored by scrum-master agent. Wave 0 dispatch is authorized.*
