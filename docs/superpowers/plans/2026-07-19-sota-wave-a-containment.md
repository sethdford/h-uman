# SOTA Wave A — Containment + Honesty Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One fail-closed tool security envelope on dispatcher, stream, DAG, and HuLa; honest SOTA docs; Turing API auth.

**Architecture:** Extract `hu_agent_internal_pre_execute_checks` (permission → hook → escalate → policy). Rewire all direct `vtable->execute` agent paths through it. Demote unmeasured SOTA claims. Auth-gate Turing HTTP routes.

**Tech Stack:** C11 agent runtime, existing `hu_permission_*`, hook pipeline, escalate protocol, policy engine, gateway `v1_auth_ok`.

## Global Constraints

- C11, `-Wall -Wextra -Wpedantic -Werror`; free every allocation; ASan clean.
- Never reference Gemini 2.0/2.5 models in docs or code.
- One concern per commit; conventional commits.
- Tests: no real network; `HU_IS_TEST` for side effects.
- Do not expand Wave A into LongMemEval training or blind A/B human runs (Wave B).

---

### Task 1: Truthfulness pass on SOTA_BENCHMARK.md

**Files:**
- Modify: `docs/SOTA_BENCHMARK.md`
- Modify: `docs/PRODUCT.md` (runtime footprint + persona module count only if touched)

- [x] **Step 1:** Demote Memory / ML-based predictions / overall Digital Twin “SOTA” where unmeasured to COMPETITIVE or PARTIAL; cite need for LongMemEval/LoCoMo.
- [x] **Step 2:** Replace Gemini 2.5 / Claude 3.7 references with Gemini 3.x / current Claude naming.
- [x] **Step 3:** Sync binary (~2974 KB measured / ~2468 KB documented claim — use measured or “~3 MB”), test count (~13447), channel count (31 catalog), persona modules (46).
- [ ] **Step 4:** Commit: `docs(sota): demote unmeasured SOTA claims and sync counts` (await user)

---

### Task 2: `hu_agent_internal_pre_execute_checks` + unit tests

**Files:**
- Modify: `src/agent/agent_internal.h`
- Modify: `src/agent/agent.c`
- Create or modify: `tests/test_tool_security_envelope.c` (or extend `tests/test_permission.c`)
- Modify: `tests/test_main.c` / CMakeLists if new file

**Interfaces:**
- Produces:
  ```c
  typedef enum {
      HU_TOOL_GATE_ALLOW = 0,
      HU_TOOL_GATE_DENY = 1,
      HU_TOOL_GATE_NEED_APPROVAL = 2,
  } hu_tool_gate_t;

  /* On DENY, *out is a fail result (caller owns). On NEED_APPROVAL, *out may be zeroed
   * and caller sets needs_approval on the eventual result. On ALLOW, *out untouched. */
  hu_tool_gate_t hu_agent_internal_pre_execute_checks(
      hu_agent_t *agent,
      const char *tool_name, size_t tool_name_len,
      const char *args_json, size_t args_json_len,
      hu_tool_result_t *out);
  ```

- [x] **Step 1:** Write failing tests: READ_ONLY agent denies shell/spawn-class tool; hook DENY populates out; policy DENY populates out.
- [x] **Step 2:** Implement pre_execute_checks ordering: permission → pre_hook → escalate DENY → evaluate_tool_policy.
- [x] **Step 3:** Extend `hu_agent_internal_dispatch_with_hooks` to call pre_execute_checks before execute (keep post_hook always).
- [ ] **Step 4:** Run targeted tests; commit: `feat(agent): canonical pre-execute security checks` (await user)

---

### Task 3: Wire streaming path

**Files:**
- Modify: `src/agent/agent_stream.c` (~2035–2150)

- [x] **Step 1:** Replace bare pre_hook-only gate with `hu_agent_internal_pre_execute_checks`.
- [x] **Step 2:** On DENY, goto stream_tool_done with result; still fire post_hook.
- [x] **Step 3:** Add/extend stream security test if harness exists; else unit-test via internal helper coverage.
- [ ] **Step 4:** Commit: `fix(agent): stream path uses full tool security envelope` (await user)

---

### Task 4: Wire DAG parallel + sequential

**Files:**
- Modify: `src/agent/agent_turn.c` (`dag_parallel_worker`, sequential batch execute ~8644–8706)

- [x] **Step 1:** Before `vtable->execute` in both paths, call pre_execute_checks; on DENY store fail result on node, skip execute.
- [x] **Step 2:** Fire post_hook after (allow or deny).
- [x] **Step 3:** Test: THREAD_SAFE tool that requires DANGER_FULL_ACCESS denied under READ_ONLY via DAG path (or synthetic unit around worker logic).
- [ ] **Step 4:** Commit: `fix(agent): DAG tool execution uses security envelope` (await user)

---

### Task 5: Wire HuLa exec_call

**Files:**
- Modify: `include/human/agent/hula.h` — add `struct hu_agent *security_agent` + setter
- Modify: `src/agent/hula.c` — `exec_call`
- Modify: `src/agent/agent_turn.c` — after `hu_hula_exec_init_full`, call setter with agent
- Modify: HuLa tests as needed

- [x] **Step 1:** Add `hu_hula_exec_set_security_agent(exec, agent)`.
- [x] **Step 2:** In `exec_call`, if security_agent set, run pre_execute_checks before execute; map DENY to HU_HULA_FAILED result.
- [x] **Step 3:** Bind from all agent_turn HuLa call sites.
- [ ] **Step 4:** Commit: `fix(hula): optional agent security envelope on CALL` (await user)

---

### Task 6: Turing HTTP auth

**Files:**
- Modify: `src/gateway/gateway.c` (`/api/turing/scores`, trend, related)
- Modify: gateway tests if present

- [x] **Step 1:** At start of each Turing handler, if `!v1_auth_ok(...)` return 401.
- [x] **Step 2:** Test unauthorized vs authorized when auth_token set.
- [ ] **Step 3:** Commit: `fix(gateway): require auth for Turing API routes` (await user)

---

### Task 7: Wave A verification

- [x] **Step 1:** `scripts/what-to-test.sh` on changed files; run those suites.
- [x] **Step 2:** Targeted Agent / HuLa / Gateway / adversarial — 0 fail (full suite optional before commit).
- [ ] **Step 3:** Update deep-audit canvas note or proof stub that Wave A exit criteria met.
- [x] **Step 4:** Open Wave B plan stub pointer in `docs/superpowers/plans/2026-07-19-sota-wave-b-measurement.md` (outline only).

---

## Wave B / C stubs (not this plan)

- **B:** LongMemEval publish, contact isolation, blind A/B gate enforcement  
- **C:** onboard polish, preference UX, wiki surface, Tier-1 depth
