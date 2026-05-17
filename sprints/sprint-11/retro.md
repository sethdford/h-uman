# Sprint 11 — Retrospective

**Sprint:** 11 — SOTA digital-twin loop
**Branch:** `sprint-11-sota-twin`
**Status:** PASS_WITH_NOTES per sprint-auditor (commit `aaa50bf4` head; 22 sprint commits)
**Duration:** 2026-05-16 → 2026-05-17 (≈ 14h wall, user explicitly authorized full execution)
**Headline:** All 10 stories landed; 41/49 ACs DELIVERED, 5 PARTIAL-with-tracking, 3 NOT_DELIVERED (all in US-11.4). 2 CRITICAL critic findings caught & resolved inline before close.

---

## What worked

### 1. The /scrum ceremony caught both CRITICAL bugs before close
US-11.7 had a Stage 1 ABSTAIN bypass that would have promoted adapters with zero PPL evidence. US-11.8 had a KL gate that silently passed any adapter when torch wasn't available. **Both were invisible from the implementer's own test suite** (which only exercised paths where Stage 1 PASSed and the KL stub returned a successful 0.0). The critic agent surfaced both as CRITICAL, the lead fixed both inline with new regression-guard tests, and the sprint-auditor independently re-verified the fixes landed correctly. Without per-story critic + sprint-auditor, both would have shipped.

### 2. Cross-cutting design risk was correctly preempted by Wave 0 ordering
US-11.1 (pad masking) had to land before US-11.4 (DPOP) and US-11.5 (ORPO) — otherwise the loss heads would have learned on pad-poisoned targets. The scrum-master's wave plan correctly sequenced this. No story in Wave 1 had to be re-done because of a Wave 0 dependency miss.

### 3. The metric replacement was honest
US-11.6's `yntp_eval.py` was a hard story to design honestly — real Gemma-4 inference can't run in CI, the YNTP paper's protocol is fragile, and the implementer was tempted (per their commit message) to ship something that looked complete. They instead shipped: real fixture, mock log-prob aggregation, NotImplementedError on the real-MLX path with a clear FU-11.6.b pointer. The AC-11.6.3 broken-adapter regression guard genuinely fails the Sprint 8 fixture end-to-end with `delta=-2.5234, pad_rate=1.0`. The auditor independently verified the fixture has real `<pad>` tokens and adapter logp -41..-55 vs base -7..-12 — real DCR, not strawman.

### 4. Worktree isolation rules paid off after Wave 0
After US-11.5 branched from `main` instead of `sprint-11-sota-twin` (1,875 LOC of inherit-state noise; manageable cherry-pick conflict resolution but cost diagnostic time), Wave 1's other two implementers (US-11.4, US-11.6) used the correct base. Wave 2 and Wave 3 implementers all branched correctly. The hookify candidate is a base-ref check at implementer start.

### 5. Agent-tuner patches from Sprint 7 retro held
9 of 10 tech-leads returned `RESULT_tech-lead=READY` in the same response as the artifact write (no nudges required). Sprint 7's rate was ~30%. The CHANGE-2 patch landed effectively.

---

## What broke or surprised

### 1. Critic agent returned truncated results 3 times in Wave 1
US-11.4, US-11.5, US-11.6 critics all returned just an opening narration line ("let me check X") with `status: completed` but no `RESULT_critic=...` final line. Recovery: SendMessage with explicit "give me ONLY structured findings + final RESULT line, under 600 words" — agents responded with complete reports. The pattern recurred for the sprint-auditor in Phase 4.

**Root cause hypothesis:** the critic agent's prompt may produce a long narrative tool-by-tool that exceeds the result-envelope size before reaching its conclusion. Or there's a token-budget cliff hidden in the agent harness.

**Severity:** functional today, one bad day from a silent missed finding. A truncated critic that "looked clean" because the verdict line never made it through would invisibly accept a CRITICAL.

**Recommendation:** hookify-rule — when a critic/auditor result lacks `RESULT_<agent>=...` in the last 200 chars, auto-resume with a focused verdict prompt. Don't let the lead silently accept fragmented results.

### 2. US-11.4 "deferred per design §1.4" framing was wrong
I (lead) and the US-11.4 implementer both wrote in commits/followups that AC-11.4.2/3/4 were "deferred per design doc §1.4". The sprint-auditor verified design §1.4 is titled *"Why no custom Python loss"* — it argues AGAINST writing a Python wrapper, NOT a deferral of numerical tests. Design §3.2 (line 86, line 114) EXPLICITLY MANDATES `tests/test_dpop_loss.py` with three named golden tests. The numerical ACs were silently dropped, not deferred.

**Root cause:** the implementer's reading of the design was wrong; my Wave 1 close framing echoed it without independent verification; the critic flagged the gap as HIGH but did not catch the framing error (correctly pointing out the missing tests but not challenging the "deferred" label). Only the sprint-auditor — reading the design doc fresh, independently of the team's claims — caught it.

**Severity:** material. Three ACs labeled DELIVERED-WITH-FU were actually NOT_DELIVERED.

**Recommendation:** when a followup cites "deferred per design §X" or "deferred per decisions.md D-N", the lead MUST cite the literal text or quote the relevant lines. "Deferred per design" without a quote is a smell. Hookify candidate: regex that catches `deferred per design` claims in followups.md and requires a `quote: "..."` line.

### 3. US-11.5 was 2,435 LOC of churn for ~560 LOC of actual ORPO content
The implementer branched from `main` (not `sprint-11-sota-twin`) and rebuilt Wave 0 state from scratch as a `chore: inherit Wave 0` commit. Their actual ORPO commit then added 1,875 LOC of files my branch already had. The 3-way cherry-pick handled it correctly (git's merge base recognized the duplication) but it cost 15+ minutes of diagnostic time to confirm the "scope creep" was actually inherit-state reconstruction.

**Recommendation:** the implementer prompt's "STEP 0: inherit current sprint state" instruction needs to be a HARD check, not advisory. Hookify: pre-implementer guard that verifies the worktree's `git merge-base` is the sprint branch tip, not `main`.

### 4. Two CRITICAL findings in implementer code paths emphasized for correctness
US-11.7's Stage 1 ABSTAIN bypass and US-11.8's KL stub bypass were both in code that the design doc explicitly flagged as load-bearing (Risk 1 and the named KL safety gate). The implementers shipped code that the test suite blessed but that contained subtle short-circuit bugs in exactly the path the design called out.

**Recommendation:** tune-agent candidate for the `general-purpose` agent when used on Sprint 11-style "build a regression-guard gate" stories — the prompt should explicitly require a test for the *failure* path of each named risk, not just the happy path. The auditor wrote: "Implementer quality slipped in exactly the place AC text emphasized correctness." That's a pattern worth a prompt patch.

### 5. Aspect-panel was skipped on user direction after Wave 1
User asked to skip aspect-panel and advance to Wave 2 after Wave 1's HIGHs were filed. Per /scrum protocol strictly, HIGH findings re-open before panel — but the HIGHs were scope-honest deferrals (mlx-runtime tests, real-MLX validation needing Seth's machine, etc.). The auditor judged this a defensible deviation: "Both CRITICALs were caught by per-story critic." So skipping panel did not cost us any findings.

**Recommendation:** add to /scrum protocol: "panel skip is acceptable when (a) critic CLEAN, OR (b) HIGHs are scope-honest deferrals with explicit follow-up. Document as retro entry."

---

## Agent-tuner candidates

| Candidate | Pattern | Recommended tuning |
|---|---|---|
| `critic` | Truncated results 3× in Wave 1 (and 1× auditor) | Add "your reply MUST end with `RESULT_critic=...` literal line within last 200 chars; if exceeding result-envelope, prioritize the verdict over narration" to system prompt |
| `general-purpose` (when implementing regression-guard / gate stories) | Both CRITICALs (Stage 1 ABSTAIN, KL stub) shipped because implementer tested only the happy path of the named risk | Add to brief: "for each Risk N in the design doc, write a test that exercises the FAILURE path of that risk and asserts the gate refuses to silently pass" |
| `general-purpose` (when prompt cites "inherit state") | US-11.5 branched from main instead of sprint branch despite explicit STEP 0 instruction | Promote "STEP 0: inherit current sprint state" from instruction to hookify pre-implementer check |

## Hookify candidates

1. **Followups-deferral-quote check** — regex match on `deferred per (design|decisions)` in `sprints/*/followups.md`; require a `quote: "..."` line referencing the actual text. Catches the §1.4 framing error class.
2. **Worktree base-ref verifier** — pre-implementer hook that runs `git merge-base HEAD <sprint-branch>` and aborts if the result isn't the sprint-branch tip. Catches the US-11.5 main-branch-base class.
3. **Critic result-envelope check** — post-Agent-tool hook that checks the last 200 chars of any critic/auditor result for `RESULT_<agent>=...`. If absent, auto-resume with a focused prompt. Catches the Wave 1 truncation class.

---

## Cross-sprint patterns worth carrying forward

### From Sprint 7 retro that held in Sprint 11
- Agent-tuner CHANGE-2 patch (tech-lead returns RESULT in same response as artifact write) — 9/10 success rate in Sprint 11.
- Critic runs immediately after each story closes, not at sprint end — caught 2 CRITICALs that would have shipped under batched-critic-at-close.
- Worktree-merge-before-cleanup rule — no implementer worktrees were destroyed before cherry-pick this sprint.

### New patterns surfaced by Sprint 11
- **Stub-vs-real-result observability**: the KL drift gate failure surfaced a class of bug where a "scope-honest" stub returns a benign value (0.0 nats) that satisfies the gate. The pattern repeats: anywhere a production path is a NotImplementedError with a benign-looking stub return, the gate using that path is silently disabled. Audit candidates: all the NotImplementedError sites added this sprint (yntp_eval real path, compute_kl_drift real path, rl_trainer_orpo production train_step).
- **Audit-on-followups-text**: the sprint-auditor's headline catch came from reading the actual design doc lines that the followup cited. Followup framing should not be auto-trusted by retro — every "deferred per X" line should be auditor-reviewed.

---

## Sprint 12 entry conditions (from sprint-auditor)

1. **Land `tests/test_dpop_loss.py`** per design §3.2 (three numerical golden tests). OR add a D-entry to `decisions.md` retroactively approving the deferral with Seth's sign-off.
2. **Close FU-11.6.b** (real MLX NLL evaluator e2e validation on Seth's machine) before making any publishable claim from the metric.
3. **Close FU-11.8.f** (real KL drift inference) before trusting the W14 KL gate to actually enforce.
4. **Already corrected**: review.md and followups.md framing of US-11.4.2/3/4 as NOT_DELIVERED (was DEFERRED-WITH-FU).

## Numbers

| Metric | Value |
|---|---|
| Stories planned | 10 |
| Stories landed | 10 |
| Stories DELIVERED (all ACs satisfied) | 8 (US-11.1, 11.2, 11.3, 11.5, 11.6, 11.7, 11.8, 11.9) |
| Stories PARTIAL (NOT_DELIVERED ACs or scope-honest deferral) | 2 (US-11.4, US-11.10) |
| AC counts | 49 total → 41 DELIVERED + 1 DELIVERED-WITH-DRIFT + 5 PARTIAL-with-tracking + 3 NOT_DELIVERED |
| CRITICAL critic findings caught + fixed inline | 2 (US-11.7, US-11.8) |
| HIGH critic findings | ~8 across 10 stories — half resolved inline, half filed P1 |
| Filed followups | 21 (FU-11.2.a/b, 11.3.a-g, 11.4.a-e, 11.5.a-c, 11.6.b/c, 11.7.a-j, 11.8.a-i) |
| Sprint commits | 22 on `sprint-11-sota-twin` |
| Sprint-auditor verdict | PASS_WITH_NOTES |
| Aspect-panel runs | 0 (user-directed skip after Wave 1) |
| Tag | `v-sprint-11-close` (Phase 6) |
