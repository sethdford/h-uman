---
title: "Sprint 54 Review — Partial close at Wave 2"
sprint: 54
branch: sprint-54-tier-1-2-3-cleanup
status: partial-close (3 of 6 stories shipped Phase 1; Wave 3 + 4 deferred)
created: 2026-05-25
last_audit: 2026-05-25
---

# Sprint 54 — Sprint Review (partial close at Wave 2)

This sprint closed Waves 1 and 2 cleanly. Waves 3 (US-C3.9 + US-C3.7
serialized on `src/doctor.c`) and 4 (US-C2.3 onboarding step) are
deferred to a follow-up session — the foundations (PO backlog, 6
tech-lead designs, scrum-master plan) remain valid and the
implementation work is well-scoped.

## What shipped (3 commits to main)

| Commit | Story | Phase | Tests | LOC |
|---|---|---|---|---|
| `fd3f0fce` | US-CLEAN-1 plan-dir frontmatter normalize | full | n/a (docs) | 125 files normalized |
| `2afef7d2` | US-C3.3 provider smoke check | **Phase 1 only** | 19/19 PASS | ~200 src + ~250 test |
| `034fb7c2` | US-M3-B4 MLX streaming wire | **Phase 1 only** | 14/14 PASS (10 prior + 4 new) | ~250 src + ~200 test |

All on `sprint-54-tier-1-2-3-cleanup`, fast-forwarded to `origin/main`
at `cd5f43de` after `git merge origin/main --no-edit` resolved 17
files of concurrent fixes (M3+M4+C1+L2 audit followups + L3
silent-config-gated-subsystems work + chatdb_readable test fixes).

## Phase 1 vs Phase 2 honesty

**US-C3.3 Phase 1 scope (what shipped):**
- Vtable wired + registered via `hu_doctor_registry_register_defaults`
- Pure error-code classifier (`hu_doctor_check_provider_classify`) mapping
  HU_OK / HU_ERR_INVALID_ARGUMENT / HU_ERR_CONFIG_NOT_FOUND / HU_ERR_NOT_FOUND
  / HU_ERR_PROVIDER_AUTH / HU_ERR_PROVIDER_RATE_LIMITED /
  HU_ERR_PROVIDER_UNAVAILABLE / HU_ERR_TIMEOUT → 7 reason variants
- Reason → kebab-case stable string for --json schema (US-C3.7 consumer)
- Reason → human-readable message for `reason` field
- run() contract: NA on NULL ctx, PASS on non-NULL (Phase 1 placeholder)

**US-C3.3 Phase 2 deferred (what's owed):**
- AC-1.2 actual 1-token `complete("ok")` smoke call against real provider
- 5 FAIL-mode tests using mock-provider failure injection
- 10s timeout test
- Wiring depends on `doctor.c::main()` switching from old dispatch to
  registry-based `run_all` (separate sprint story).

**US-M3-B4 Phase 1 scope (what shipped):**
- `mlx_supports_streaming` + `mlx_stream_chat` wired in vtable
- HU_MLX_SUBPROCESS_ACTIVE-gated full streaming subprocess driver
  (~250 LOC): fork+pipe+exec `python3 -u -m mlx_lm.generate`, select-driven
  non-blocking reads, whitespace-boundary chunk emission, **UTF-8-safe
  tail buffering**, cancellation via callback-returns-false → SIGTERM →
  waitpid, 180s timeout protection, final is_final=true chunk on clean
  exit
- 4 new tests: supports_streaming=false in test build, vtable wiring,
  NOT_SUPPORTED + callback-never-invoked invariant, NULL arg rejection

**US-M3-B4 Phase 2 deferred (what's owed — gated on real MLX runtime):**
- `test_mlx_stream_chat_chunks_equal_batch` — chunks-sum-to-batch
  determinism on a fixture model
- `test_mlx_stream_chat_cancellation_terminates_subprocess` — real
  cancellation kills the python subprocess within 1s

Both Phase 2 tests need a fixture model on disk + a stable mlx_lm seed,
which means they ship in a session where that fixture is available
(typically on the Apple Silicon dev box, not default CI).

## What's NOT done (Waves 3 + 4)

Designs are complete; implementation deferred to fresh session.

| Wave | Story | Why deferred |
|---|---|---|
| 3 | US-C3.9 doctor exit-code contract (~230 LoC) | Smallest remaining; would be the natural next inline target |
| 3 | US-C3.7 doctor `--json` output (~350 LoC) | Must serialize after US-C3.9 (both modify `src/doctor.c`) |
| 4 | US-C2.3 onboarding provider step (~700 LoC) | Largest; depends on US-C3.3 Phase 2 for the actual smoke reuse |

## Process notes (for the retro)

### What worked

1. **PO + 6 tech-leads + scrum-master phase** delivered ~50KB of design
   artifacts in ~10 min of agent-time. The plan + designs survived the
   chaos of Wave 1 implementer's scope violation; they remain the
   source of truth for the deferred work.

2. **Sequential wave execution** (Wave 1, then Wave 2 sequentially even
   though plan said parallel-safe) avoided the CMakeLists.txt collision
   that wiped earlier sessions. Each story committed atomically with
   scope-verification before commit.

3. **Phase 1 / Phase 2 scoping** kept the sprint shipping when AC-1.2
   couldn't be met inline (no mock-provider failure injection wired
   yet; no MLX fixture model on disk). The deferred work is enumerated
   so Phase 2 is mechanical, not exploratory.

4. **The "no allow-silent-pass" discipline** held: 19+4=23 new tests
   all assert real contracts (no `HU_ASSERT_TRUE(1)` tautologies, no
   conditional-on-exists test gates). Per
   `.claude/rules/tests-that-pin-bugs.md`.

### What broke

1. **Wave 1 implementer agent scope violation**: the first US-CLEAN-1
   implementer attempt (commit `d1b7dfac`, since reset) committed not
   only the 125 docs/plans/ normalizations but ALSO 5 unrelated C-file
   reverts (src/ml/dpo.c, src/providers/gemini.c, etc.) including the
   production DPO bug fixes. Caught by the post-DONE `git diff
   --name-only HEAD~1..HEAD | grep -v ...` scope check before merge.
   Recovered via cherry-pick to extract only the docs/plans changes
   (commit `fd3f0fce`).

2. **Tech-lead agents returning mid-stream**: 5 of 6 design-doc agents
   produced visible "Now let me check..." final messages that LOOKED
   truncated. Actual file inspection showed the design WAS written
   before the agent stopped — the visible tail was buffered output, not
   a true mid-step halt. US-M3-B4 was the exception (agent stopped
   pre-write); retry with stricter "write first, refine after"
   instruction succeeded.

3. **Test harness `--suite=X` filter syntax**: passing multiple
   `--suite=A --suite=B` flags doesn't always behave as expected. Use
   `--filter="A|B"` regex syntax for cross-suite runs. Not a bug worth
   fixing in this sprint; documented in retro.

### What changed for next sprint

1. **All implementer prompts include the scope-verification step
   explicitly**: "After commit, run `git diff --name-only HEAD~1..HEAD
   | grep -v <allowed paths>` — if non-empty, your commit is rejected
   and you must squash-fix to only allowed paths." (Already in the
   scrum protocol; need to surface it more loudly in implementer
   briefs.)

2. **Phase 1 / Phase 2 as a first-class scoping primitive in
   stories.md**: when an AC can't be met inline (network smoke,
   fixture model required), stories.md should pre-split into Phase 1
   (structural) + Phase 2 (runtime). The product-owner does this work;
   tech-leads design both phases.

3. **Inline foreground execution for stories ≤ 400 LoC**: the
   Wave 1 chaos taught us that implementer agents are a poor fit when
   the work is mechanical and the scope is tightly bounded. Sprint 49
   used agents because each story was ≥ 600 LoC; this sprint's Wave 2
   stories shipped cleanly by hand. The /scrum protocol should permit
   "scrum-master does the implementation directly" as an explicit
   wave-execution mode for small stories.

## Audit + retro

- **No `sprint-auditor` adversarial pass run yet** — that's the next
  Phase 4 step. The auditor reads stories.md + this review + every
  commit's actual diff and decides whether each AC was honestly
  delivered. The Phase 1 caveats here are explicit so the auditor
  knows what NOT to expect.
- **No `/mine-transcripts` retro pass** — also Phase 5 work. The "what
  changed for next sprint" section above is the manual seed.

## Close

Sprint 54 closes PARTIAL at Wave 2 / 3 stories shipped / 3 stories
deferred (with full designs available for fresh-session pickup). All
shipped work is on origin/main; the sprint-54 worktree is safe to
keep or remove at the user's discretion.

Tag (when sprint formally closes): `v-sprint-54-wave-2-close` pointing
at `cd5f43de` (or whatever HEAD is at that moment).
