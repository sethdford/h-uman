# Sprint sprint-better-than-human-2026-09-05

- branch: sprint-better-than-human-2026-09-05
- worktree: /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-better-than-human-2026-09-05
- base: d5c0257b8ea682001eb8fc7d8daf334d15ac8094 (origin/main at planning time)
- isolation: worktree (27 worktrees, ~10 concurrent sessions)
- goal: six measured levers toward "better than human" — see stories.md

## 1. File-conflict matrix

Built from every design's own "Files touched" + "Conflicts" section (all 8 designs
were read; each design independently self-verified against `stories.md`). Only
entries with 2+ stories are listed; every other touched file is single-story and
carries no conflict.

| File | Stories | Nature of overlap | Resolution |
|---|---|---|---|
| `scripts/eval_when_to_speak.py` | US-3, US-4 | US-3 adds a 2-line `FIR_WINDOW_HOURS` constant + argparse rewire; US-4 rewrites the join/dedup logic (~50-70 LOC) elsewhere in the file. Disjoint line ranges, but same file. | **Sequential: US-3 before US-4** (US-4's AC-4.4 also needs US-3's baseline output, so the ordering is a real dependency, not just a file lock). |
| `.github/workflows/ci.yml` | US-1, US-7 | Both add one line to the same `capability-gate-check` job, same region (ci.yml:953-971). **Not called out in stories.md — found by reading both designs.** | **Sequential: US-1 before US-7** (US-1 is P0; US-7 rebases past a 1-line diff, same pattern as the US-3/US-4 case). |
| `scripts/eval_persona_evolution.py` | US-1 (read-only import of `dedup_key`/`TAPBACK_PREFIXES`), US-7 (edits: `--trailing-days` mode) | **Not a write conflict** — US-1 never edits this file (confirmed in US-1 §3 "Not touched" and §7). | No lock needed; both designs independently confirm zero conflict. Land in either order; US-7's implementer does a 1-line sanity check that `dedup_key`/`TAPBACK_PREFIXES`/`starts_lowercase`/`terminal_punctuation` are untouched. |
| `scripts/blind_ab/score.py` (`wilson()`) | US-2 (n/a — doesn't touch it), US-3 (read-only import), US-6 (read-only import, contract frozen) | Both US-3 and US-6 import `wilson()` but neither edits it; both designs explicitly commit to leaving its signature unmodified. | No conflict, no lock. |
| `src/memory/retrieval/hybrid.c`, `src/memory/semantic_recall.c` | US-5 only | — | No conflict. |
| `src/agent/model_router.c`, `include/human/agent/model_router.h` | US-8 only | — | No conflict. |
| `scripts/eval_semantic_live_gate.py` | US-5 (writes: `--register-gate` flag), US-8 (design says "extend... **or** a new script" — explicit either/or, not committed) | **Soft/future conflict only**, flagged by US-5's own design, not a same-sprint collision unless US-8's implementer picks "extend" in parallel with US-5. | Wave ordering already puts US-5 before US-8 (see §2); US-8's implementer reads the merged `register_breakdown` shape before choosing extend-vs-new-script. |
| `scripts/blind_ab/authorship_gap.py` | US-2 (reads its JSON output), US-8 (extracts `twin_score()` helper, pure refactor) | No write overlap in this sprint (US-2 does not edit `authorship_gap.py` itself per its design; it reads committed JSON). | No lock; soft sequencing only (US-8 benefits if US-2's gate has already landed, not required). |

**Everything else is single-story:** `scripts/merge_seth_preference_sources.py` (US-1
only), `scripts/blind_ab/authorship_promotion_gate.py` / `m3_promote.py` /
`register_v6_adapter.py` / `nightly-retrain.sh` (US-2 only), new
`scripts/eval_seth_initiation_baseline.py` (US-3 only), `scripts/blind_ab/make_rating_sheet.py`
/ `score_preference.py` / `PROTOCOL.md` (US-6 only), `docs/plans/2026-09-02-persona-evolution/spec.md`
(US-7 only, edits — US-1 never edits it either).

## 2. Waves

Wave 0 dispatches exactly ONE implementer to prove worktree isolation actually holds
in this environment before any fan-out, per `~/.claude/rules/worktree-isolation-lifecycle.md`
(the flag is advisory, not guaranteed — verify with `git worktree list` + a clean
`git status` in the shared checkout before trusting parallel dispatch). US-1 is the
natural candidate: P0, zero technical dependencies, zero file conflicts with anyone.

| Wave | Stories | Why this grouping |
|---|---|---|
| **W1** | US-1 (solo) | Isolation-proof dispatch. P0, no dependencies, no file conflicts. Confirm `git worktree list` shows a new worktree AND the shared checkout stays clean before proceeding to W2. |
| **W2** | US-2, US-3, US-6, US-7 (parallel, 4-way) | All Python-only. Zero file conflicts among the four (verified above and independently confirmed by each design's own Conflicts section). The two sequencing constraints from §1 that touch W1 (US-1→US-7 on `ci.yml`, and the non-conflict on `eval_persona_evolution.py`) are already satisfied because US-1 merges before W2 starts. |
| **W3** | US-4 (Python-only) + US-5 (C-only) | US-4 requires US-3 merged (`eval_when_to_speak.py` shared-file + AC-4.4 dependency) — W2 satisfies this. US-5 is a C-code story with zero technical dependencies; paired with a Python-only story per the C-code isolation constraint (never two C-code stories in the same wave). No file overlap between US-4 and US-5. |
| **W4** | US-8 (solo, C-only) | C-code story; nothing Python-only remains unscheduled to pair with, so it runs alone (also satisfies the constraint). Soft dependency on US-2's `twin_score()` extraction is already merged from W2. |

`RESULT_scrum-master=PLAN_READY` wave list: **W1: US-1 | W2: US-2, US-3, US-6, US-7 | W3: US-4, US-5 | W4: US-8**

## 3. Per-story dispatch

Every implementer is `general-purpose`, `isolation: worktree`, model `sonnet` unless
noted — none of the eight tech-lead designs flagged a heavy-novel-C-reasoning need
(US-5 and US-8's C changes are both small, ~20-40 LOC, and the design already
specifies the predicate/call-site verbatim; escalate to `opus` only if the
implementer reports genuine ambiguity, e.g. an ASan false-positive matching
`.claude/rules/asan-pthread-stack-aliasing-darwin.md`).

| Story | Wave | Est. | Files (C?) | Notes |
|---|---|---|---|---|
| US-1 | W1 | M | Python only | Corpus merge + rebalance. No model load — `check-no-resident-model.sh` must stay green throughout. |
| US-2 | W2 | M | Python only | AC-2.6 needs one real nightly window's JSON — implementer stages the gate, then waits for/captures the next `HU_RETRAIN_MLXTUNE` window's output; do not force an out-of-window run. |
| US-3 | W2 | S | Python only | Read-only `chat.db` (`mode=ro&immutable=1`) — verifier must confirm zero write statements. |
| US-6 | W2 | M | Python only | AC-6.5's real n≥20 run needs human rater time (calendar, not engineering) — implementer stages the pipeline; scrum-master schedules the rater pass separately. |
| US-7 | W2 | S (design revised down from stories.md's M — confirmed reuse-only) | Python only | Also lands one prose section in `spec.md` (§8) — non-code deliverable, still needs verifier to check numbers are cited correctly, not just that the file compiles/parses. |
| US-4 | W3 | S | Python only | Blocked until US-3's PR is on the sprint branch (needs the merged `FIR_WINDOW_HOURS` symbol to exist for import, and needs US-3's baseline number for AC-4.4). |
| US-5 | W3 | L | C: `src/memory/semantic_recall.c/.h`, `src/memory/retrieval/hybrid.c` (call site only), `tests/test_semantic_recall_register.c`, `CMakeLists.txt`, `tests/test_main.c` | Register-gated LIVE recall predicate. Ships default-OFF behind `HU_SEMANTIC_RECALL_REGISTER_GATE`; `:8741` is never restarted. |
| US-8 | W4 | L | C: `include/human/agent/model_router.h`, `src/agent/model_router.c`, `tests/test_model_router.c`, `tests/test_agent_turn_persona_head_shared.c` (new) | SHADOW-only routing log. `src/daemon.c` explicitly not touched — verifier must diff-check this. |

### Closing-line contract (mandatory, every implementer)

Every implementer's final message MUST end with this block verbatim (fields filled
in, keys unchanged) — no other closing format is accepted as evidence of DONE:

```
RESULT_implementer-<story>=DONE|PARTIAL|BLOCKED
commits: <sha[,sha...] on the sprint branch>
build-exit: <int, or N/A for python-only stories with no C touched>
test-exit: <int>
test-summary: <verbatim last line(s) of the actual test run — "Results: N/N passed"
  for C suites, the actual pytest/unittest summary line for Python — never paraphrased>
git-log-evidence: <verbatim output of `git -C <worktree> log --oneline -1`>
asan: <clean | N errors (paste the ASan report head) | N/A (python-only, no C touched)>
```

**DONE requires a commit reachable from the sprint branch's tip**, verified the same
way as any worktree dispatch (`~/.claude/rules/worktree-isolation-lifecycle.md`):
`git -C <worktree> cat-file -t <reported-sha>` must succeed, and
`git -C <worktree> log --oneline -1` must show that sha as the tip. A PARTIAL/BLOCKED
report with no commit is acceptable and honest; a DONE report with no commit, or a
commit not reachable from the branch, is rejected and re-dispatched — never trusted
on the implementer's word alone, per `~/.claude/rules/ground-truth-over-proxy-signals.md`.

## 4. Gate sequence (every story, no exceptions)

```
implementer (writes code + commits)
   -> verifier        (runs the actual code: build, test suite, or hermetic pytest
                        run for Python-only stories; reports RESULT_verifier=PASS|FAIL|INCONCLUSIVE)
   -> critic           (dispatched as a general-purpose agent given the critic role's
                        prompt/checklist verbatim — the TYPED `critic` agent caps out at
                        12 turns and this sprint's designs are large enough to exceed
                        that; general-purpose has no such cap)
   -> aspect-panel      (skill: 5-dimension confidence-weighted panel — correctness,
                        edge-case, security, regression, style; run via `/aspect-panel`)
   -> mark DONE         (scrum-master, only after verifier PASS + critic has no
                        outstanding CRITICAL finding + aspect-panel has no <40%
                        pass-share dimension unresolved)
```

Max 2 critic-round trips per story (`~/.claude/rules/agent-team-os.md` cap) — a third
failed round means the story is mis-scoped for this sprint; split it or escalate to
the stakeholder rather than re-reviewing again.

## 5. Hard constraints (apply to every wave, every story)

- **Never restart or repoint `:8741` or the service-loop.** Every design confirms
  this by construction (US-5 AC-5.5, US-4 AC-4.6, US-8 AC-8.6) — verifier must check
  this explicitly for US-5/US-8/US-2's nightly-window story, not just take the
  implementer's word.
- **Model loads happen only inside the nightly `HU_RETRAIN_MLXTUNE` window**, gated by
  `scripts/check-no-resident-model.sh`. No story in this sprint trains or loads a
  second Python LLM instance outside that window (`~/.claude/lessons.md`:
  "NEVER two Python LLM instances at once").
- **No private text, phone numbers, or contact names committed to the repo.** Every
  story's evidence artifact is aggregate counts/rates/JSON only — verifier greps every
  new `sprints/.../evidence/*.json` file for phone-shaped and name-shaped strings
  before accepting DONE (US-1 AC-1.5, US-3 AC-3.5, US-6 AC-6.4, US-7 AC-7.7).
- **`src/daemon.c` stays at or under its 12313-LOC ratchet ceiling** (confirmed at
  exactly 12313 today). US-5 and US-8 explicitly do not touch this file — verifier
  runs `wc -l src/daemon.c` before and after each C-touching story's commit.
- **All `scripts/check-*.sh` ratchets stay green** — file-size-ceiling,
  sqlite-includer-ratchet, clone-ratchet, no-new-root-files, agent-core-boundary,
  modeled-person-layering, edge-context-isolation, test-source-gate-symmetry. US-5's
  own design pre-verifies most of these by construction; verifier still runs the
  actual scripts, not just reads the design's claim (`~/.claude/rules/verify-before-you-claim.md`).
- **Every send-path-adjacent feature ships OFF→SHADOW, never LIVE, this sprint.**
  US-5's register gate and US-8's routing log are both explicitly default-OFF/SHADOW
  per `.claude/rules/feature-gate-requires-measurement.md` — no PR in this sprint
  flips a gate to LIVE or changes `HU_SEMANTIC_RECALL`'s existing LIVE default.

## 6. Standup format

Posted once per wave transition (not daily — this sprint is wave-paced, not
calendar-paced). Each line:

```
<story> [<wave>] status=<not-started|in-progress|verifier|critic|aspect-panel|DONE|BLOCKED>
  commit=<sha or "none yet">  blocker=<one line or "none">
```

Scrum-master calls out explicitly at each standup:
1. Any story whose gate sequence (§4) has been open for >2 wave-transitions without
   reaching DONE — that's a signal to split or escalate, not to keep waiting.
2. Any ratchet or hard-constraint (§5) violation surfaced by a verifier or critic run.
3. Whether the next wave's file-conflict prerequisites (§1) are actually satisfied
   (merged, not just "implementer says done") before dispatching it.

## 7. Definition of Done checklist (per story, before scrum-master marks DONE)

- [ ] Closing-line contract (§3) posted verbatim, with a commit sha
- [ ] Commit sha confirmed reachable from the sprint branch tip (`git log --oneline -1`)
- [ ] `RESULT_verifier=PASS` (not FAIL, not INCONCLUSIVE — INCONCLUSIVE blocks the same as FAIL)
- [ ] Critic reviewed at least once; no outstanding CRITICAL/HIGH finding
- [ ] Aspect-panel run with no dimension left at a 40-60% disagreement band unresolved
- [ ] All acceptance criteria in the story's `stories.md` entry individually addressed
      (not "tests pass" as a proxy — each AC checked against the actual artifact)
- [ ] Every applicable hard constraint in §5 re-confirmed for this specific story
- [ ] No new `#include <sqlite3.h>`, no new loose `src/*.c` root file, no vtable/
      bounded-context boundary violation introduced (ratchets stay at or below baseline)
- [ ] Evidence file (if any) committed under `sprints/sprint-better-than-human-2026-09-05/evidence/`
      with aggregate-only content, reviewed for privacy per §5

RESULT_scrum-master=PLAN_READY — waves: W1: US-1 | W2: US-2, US-3, US-6, US-7 | W3: US-4, US-5 | W4: US-8
