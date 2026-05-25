# Sprint 55 Retro — M3 SOTA Personalization

**Date:** 2026-05-25
**Branch:** sprint-55-m3-sota (8 commits)
**Delivered:** 7/9 stories (US-1..US-7); US-8 + US-9 carryover to Sprint 56
**Tests:** 12011/12011 + 4 skipped; ASan CLEAN

## What worked

### Critic-finding flow

Wave 1 critic returned `HAS_FINDINGS_0_2` flagging both:
- US-1's arbitrary `≥10 bytes` threshold (AC said "non-empty" — a real reply like "ok" at 2 bytes would have failed)
- US-3's `±2 token` tolerance (greedy mode should be exactly deterministic — the tolerance papered over a contract violation)

Both findings were correct, both were addressed inline at commit `1226c187`, and the sprint advanced without dispatching another implementer. This is exactly what the critic role exists for. **Worth repeating in Sprint 56.**

### Story bundling for shared-file dependencies

The PO proposed 5 waves over 6 days. The SM noted that Wave 1's US-1/2/3 all touch `tests/test_mlx_provider.c` — "sequential due to file conflict." Bundling all three into a SINGLE implementer dispatch (one worktree, one commit) avoided merge conflicts and used fewer agent dispatches. Same pattern worked for Wave 2b (US-5 + US-6, both in test_mlx_provider.c). **Codify: when N stories share a file, bundle them per agent.**

### Lightweight verification when full verifier+critic dispatches time out

Sub-agent timeout was a real pattern (see below). For stories with clear DoD (build-exit=0, test count, ASan log) the lead's manual grep-based checks substituted adequately. **The implementer's own report IS evidence-grade if it includes fresh build + test exit codes.** Strict-protocol verifier dispatch is most valuable for stories where the implementer's run might not reflect reality (e.g. cross-platform behavior).

### User-revised spec mid-sprint

The user rewrote `docs/plans/2026-05-25-director-compression/{requirements,design,tasks}.md` while Sprint 55 was running. The user's revision was BETTER than my draft: tighter ACs, accurate field count (27 vs my mistaken 40 — I was double-counting), correct module-name alignment with the actual design intent (`prompt_budget` not `director_compression`), added silent-failure diagnostic AC. **Lesson: PO/lead should explicitly invite user revision of specs at the review checkpoint, not just at the question-and-answer phase.**

## What didn't work

### Sub-agent budget timeout — ~30% rate

Dispatched 9 tech-leads in parallel: 5 cleanly returned RESULT_=READY, 4 ran out of chat budget mid-task. The 4 that timed out had all written their full design docs to disk — the truncation was in the chat response, not the work. Same pattern hit verifier + critic on Wave 1; SendMessage continuation recovered the critic's verdict but lost compute time.

**Root cause:** dispatching N=9 specialists with rich source-reading prompts each at the same chat-budget tier. Even "read-only" Opus dispatches max out around 60-80K tokens when they trace through C source files.

**Lesson for next sprint:**
- Cap parallel dispatches at 5 per batch
- Pre-warm specialists with a smaller scoping pass that reduces per-agent context needs
- Use SendMessage resume for "wrap up your verdict" rather than re-dispatching

### Test_ml.c clang-tidy diagnostics misattribution

Multiple times during the sprint, new clang-tidy warnings surfaced in `tests/test_ml.c` (implicit-widening, case-style) and I initially treated them as Wave 2 regressions. They were pre-existing — surfaced because the file recompiled, not because Sprint 55 touched those lines. Lost ~10 min triaging.

**Lesson:** before treating a new lint warning as a regression, `git log -L <line>,<line>:<file>` to see if the offending lines were modified by sprint commits. Codify in `~/.claude/rules/lint-noise-vs-regression.md` if recurring.

### Empirical proof of M3 mission deferred

US-5's strategic test exists structurally but SKIPS under `HU_IS_TEST` because the MLX subprocess returns `HU_ERR_NOT_SUPPORTED` in test builds. The test infrastructure satisfies AC but does not empirically prove that v4-repair adapter biases output. That proof properly belongs to US-9 (eval harness) — which is carryover. **The M3 SOTA claim therefore remains UNPROVEN at sprint close.**

This is honest scope management (US-9 is the right home for the empirical proof) but worth flagging: 7/9 stories is a real accomplishment AND the headline mission still awaits Sprint 56's eval harness.

### Closing-ceremony agent budget

Sprint-auditor ran out of budget mid-investigation. Sprint-review completed cleanly. Pattern matches the tech-lead timeout: large-context read-heavy agents hit the wall. Resumed via SendMessage to extract verdict.

## What to change in Sprint 56

1. **Smaller agent batches.** Cap parallel specialist dispatch at 5. For larger sets, run two batches sequentially.
2. **Pre-scope before deep dives.** Send a lightweight "what files do you need to read" pass first; then dispatch the full design with the trimmed file list.
3. **Bundle stories that share files.** PO sequencing of stories shouldn't blindly map to implementer dispatches; lead should re-bundle by file scope.
4. **Distinguish lint noise from regressions automatically.** Add a pre-commit check that runs clang-tidy ON CHANGED LINES ONLY, not the whole file.
5. **Bias-direction observability for US-5 type tests.** When the strategic test skips, log a one-shot WARN at sprint review time saying "M3 mission claim is deferred to US-9 eval — do not claim SOTA until that runs."
6. **Pick up US-8 + US-9 first in Sprint 56.** Carryover should have wave-1 priority; otherwise it stays carryover forever.

## Tune-agent candidates

Per protocol, agents with ≥2 verifier failures or recurring issues become `/tune-agent` candidates. From Sprint 55:

- **tech-lead**: 4/9 dispatches timed out without emitting RESULT_. Not a quality failure — a budget-sizing failure. Tune the agent's prompt to: (a) commit findings to disk INCREMENTALLY during work (so disk reflects progress even if chat is truncated), (b) emit the RESULT_ line FIRST before any verbose summary.
- **sprint-auditor**: timed out at 76K tokens deep into independent verification. Tune: cap initial reads to N files, defer extras to SendMessage round trips, ensure audit.md is written incrementally.

Both worth filing as `/tune-agent` proposals after the retro.

## Sprint metrics

- **Total commits on sprint branch:** 8
- **Total agent dispatches:** ~22 (1 PO + 9 tech-leads + 1 SM + 3 implementers + 1 verifier + 1 critic + 1 review + 1 audit + 5 background-resume attempts)
- **Stories delivered:** 7 / 9 (78%)
- **Strategic-mission test (US-5):** structurally complete; empirically deferred to US-9
- **New production code:** mlx.c safetensors validation (US-6), latency_ms wire across providers (US-4), fidelity delta function (US-7)
- **Estimated agent-cost:** ~$8-12 (Opus-heavy tech-leads + verifier + auditor; not budgeted formally)
- **Wall-clock:** approximately 2.5 hours from /scrum invocation to retro

## Carryover docket for Sprint 56

1. **US-8** — Phase C3 training_loop.py --source-jsonl wiring. Design at `designs/US-8.md`. ~450 LoC across scripts + tests. Estimated 1-2 days.
2. **US-9** — Nightly eval harness + SOTA gate. Design at `designs/US-9.md`. Depends on US-7 (delta function, landed) + US-8 (training loop, carryover). Critical for actually proving the M3 mission claim.
3. **Apply pre-existing test_ml.c lint debt.** Separate spec; not blocking, not in Sprint 56 unless capacity allows.
4. **Tune-agent on tech-lead + sprint-auditor.** See above.

## End of retro
