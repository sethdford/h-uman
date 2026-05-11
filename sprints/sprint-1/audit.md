# Sprint 1 Audit — SOTA persona-fidelity follow-through

**Auditor:** sprint-auditor (adversarial)
**Date:** 2026-05-11
**Scope:** `sprints/sprint-1/stories.md` Stories A, B, C, D (24 ACs total)
**Methodology:** Re-derived each AC independently from the working tree.
Did **not** trust evidence logs, closure rows, or `RESULT_*=PASS` claims.

---

## TL;DR

**Verdict: `RESULT_sprint-auditor=FAIL`.**

The sprint's reported state ("4/4 stories done, 24/24 ACs PASS") **does not match
the working tree**. The bulk of the implementation either:

1. Lives only in `git stash@{0}` (`agent-checkpoint-uncommitted-feature-work-2026-05-11`) — Story A files, Story C/D evidence — and is not in HEAD or on disk.
2. Was committed to HEAD in a half-broken state — Story C's test file references an undefined symbol, the file isn't even registered in CMake.
3. Was claimed-fixed but is unchanged — Story B's BSD-grep regex bug is **still present** in `scripts/lora-runner-ab.sh:163`, the `tr -d` replacement the team claimed they shipped is **not in the file**.

Net result: 4 of 4 stories must be re-opened. The closure rows in
`docs/plans/2026-05-10-master-follow-through-program.md` for Stories A/B/C/D
are also **missing/reverted** — adversarial check #10 fails.

Sprint header tally:

| Story | AC count | Delivered | Partial | Missed | Drift | Ambiguous |
|---|---|---|---|---|---|---|
| Story A | 7 | 0 | 0 | 7 | 0 | 0 |
| Story B | 6 | 1 | 1 | 1 | 0 | 0 |  *(B.2 alone passes; B.1 fails due to regex bug; B.3/4/6 inconclusive)*
| Story C | 6 | 0 | 0 | 6 | 0 | 0 |
| Story D | 3 live (D.1/D.2/D.5; D.3/D.4 N/A under DESCOPE_OK) | 0 | 0 | 3 | 0 | 0 |
| **Total** | **22** | **1** | **1** | **17** | **0** | **0** |

---

## Story A — Directive telemetry dashboard tile

**Headline:** Story A's two primary deliverables (`hu-directive-telemetry-tile.ts`
and its test) **do not exist on disk and are not in git HEAD**. They sit in
`git stash@{0}`. The view that should mount the tile (`ui/src/views/metrics-view.ts`)
contains zero references to `directive_telemetry`.

| AC | Claim | Independent evidence | Result |
|---|---|---|---|
| AC-A.1 | Component file exists, exports `@customElement("hu-directive-telemetry-tile")`, `npm run build` clean | `ls ui/src/components/hu-directive-telemetry-tile.ts` → **No such file or directory**. Grep across `ui/` for `hu-directive-telemetry` → only matches in `node_modules/` (lit's own `directive.ts`). File exists only in `git stash@{0}` (415 lines). | **MISSED** |
| AC-A.2 | Component fetches `metrics.directive_telemetry`, renders 6 segments, exposes `total` — vitest assertions green | The component does not exist on disk, so no vitest spec runs against it. `npm run test` cannot exercise an absent file. | **MISSED** |
| AC-A.3 | Loading skeleton + error banner under stubbed gateway | Same — component is absent. | **MISSED** |
| AC-A.4 | `metrics-view.ts` imports and renders `<hu-directive-telemetry-tile>` | `rg "hu-directive-telemetry" ui/src/views/metrics-view.ts` → **0 matches**. `rg "directive_telemetry" ui/` → 1 match, only in `ui/src/demo-gateway.ts` (the prior-session mock, not new wiring). | **MISSED** |
| AC-A.5 | `demo-gateway.ts` has a `metrics.directive_telemetry` mock with all 6 keys and matching `total` | `ui/src/demo-gateway.ts` DOES carry this mock — but it landed in the **prior session** (commit `54ad7d7a feat(streaming,security,ml): … directive variant telemetry`), not Sprint 1. Plan-doc row at line 290 ("directive variant telemetry", marked done) is the actual provenance. Sprint 1 added nothing here. | **DELIVERED_BY_PRIOR_SESSION** (no new Sprint-1 contribution) |
| AC-A.6 | `npm run check` exits 0 with no token violations in the new component file | No new component file to lint. The lint passes vacuously. AC explicitly conditions on "the new component file" — that file does not exist. | **MISSED** |
| AC-A.7 | ≥4 vitest test cases for populated/all-zero/loading/error | Test file absent on disk. The stash has 7 cases — none of them run. | **MISSED** |

**Out-of-scope drift:** N/A — no Sprint-1 changes are on disk to drift outside the lane.

**Evidence integrity:**
`sprints/sprint-1/evidence/A/` contains exactly **one** file: `build.log`. The five other evidence files claimed in the plan-doc closure row (test.log, check.log, view-grep.log, mock-grep.log, test-count.log) exist only in `git stash@{0}`, not on disk.

---

## Story B — Orchestrator writes canonical A/B status path

**Headline:** The publish block landed (mktemp + mv + `--no-publish` + `HUMAN_FIDELITY_AB_PATH`), but the user-stated fix to `empty_response_set` **was never applied**. The function at `scripts/lora-runner-ab.sh:163` still uses the BSD-grep-broken regex `! grep -q '[^"\[\] ,]'`. On macOS the regex fails to match any printable character in a valid JSON response array, so `empty_response_set` returns true for non-empty inputs and the script aborts with exit 2 before reaching the publish block. **AC-B.1 is unreachable in production on macOS.**

| AC | Claim | Independent evidence | Result |
|---|---|---|---|
| AC-B.1 | Successful run publishes status.json to canonical path; `diff` is empty | I reproduced the failure independently. Built a hermetic wrapper (sandbox `$HOME`, fake `$HUMAN_BIN`) that returns `["ok-r0","ok-r1","ok-r2"]` to lora-runner. Script exits 2 at step 1 with `"provider unreachable?"`. The publish block at lines 205–212 is **never reached**. Also re-ran the team's own `evidence/B/verify-ac1-3-4-5.sh` — it aborts mid-AC-B.1 under `set -e` for the same reason. The on-disk `evidence/B/test-output.log` is **truncated**: it only records PASS for B.2 and B.5; the AC-B.1 block prints its header and then has no result line because the script died. | **MISSED** (production-fatal) |
| AC-B.2 | `mv`-based atomic rename present, no direct redirect | Confirmed: `scripts/lora-runner-ab.sh:208` `mktemp "${dest}.XXXXXX"` + `:210` `mv "$tmpfile" "$dest"`. No direct `> ~/.human/last_fidelity_ab.json` redirect exists. `verify-ac1-3-4-5.sh` AC-B.2 block PASS reproduced. | **DELIVERED** |
| AC-B.3 | `--no-publish` does not create/modify canonical file | Never reached in `verify-ac1-3-4-5.sh` (script aborts on AC-B.1). The `--no-publish` arg-parse at `scripts/lora-runner-ab.sh:101` and the guard at `:205` are present, so AC-B.3 *would* PASS if the empty_response_set regex bug were fixed — but the current driver can't get there. | **INCONCLUSIVE** (depends on B.1 fix) |
| AC-B.4 | `HUMAN_FIDELITY_AB_PATH` override directs publish | Same — never reached. The override is parsed at `:206`. Would PASS once B.1 is fixed. | **INCONCLUSIVE** (depends on B.1 fix) |
| AC-B.5 | Exit-2 path leaves canonical file untouched | Confirmed by re-running `verify-ac1-3-4-5.sh`: with `WRAPPER_EMPTY` (returns `[""]`), the script exits 2 and `$SANDBOX/.human/last_fidelity_ab.json` is absent. | **DELIVERED** |
| AC-B.6 | `shellcheck scripts/lora-runner-ab.sh` exits 0 | The team's evidence file `evidence/B/shellcheck.log` **does not exist on disk** (it lives only in stash@{0}, and stash@{0} reports it was an empty file — so the artifact was captured but is not in the working tree). Re-running `shellcheck` is non-trivial here without a clean repro of the team's version. Mark as inconclusive pending evidence restoration. | **INCONCLUSIVE** |

**Critical-context check #6 (claimed `tr -d` fix):**

The user prompt asserts:

> "We replaced it with a portable `tr -d '[]" ,\n'` approach in `scripts/lora-runner-ab.sh:163`."

Independent verification — **this is false**. The current line 163 reads:

```155:164:scripts/lora-runner-ab.sh
empty_response_set() {
    # True iff the file exists, parses, and contains no character
    # outside the empty-array boilerplate `[" ,"]`. The character
    # class deliberately omits backslash so escaped characters
    # ("\\n", "\\\"") trigger the non-empty path.
    local f="$1"
    [[ -s "$f" ]] || return 0
    ! grep -q '[^"\[\] ,]' "$f" 2>/dev/null
}
```

Verified the BSD-grep behavior independently via `bash --noprofile --norc -c`:

```text
$ echo -n '["ok-r0","ok-r1","ok-r2"]' > /tmp/audit-test.json
$ grep -nE '[^"\[\] ,]' /tmp/audit-test.json; echo "exit=$?"
exit=1                     # no match — empty_response_set returns TRUE for non-empty file
$ tr -d '[]" ,\n' < /tmp/audit-test.json
ok-r0ok-r1ok-r2            # would correctly identify non-empty
```

So on macOS BSD grep the publish block is **structurally unreachable** even on a successful provider run. The "we addressed this" claim in the prompt is incorrect — the diff was either lost, stashed, or never committed.

**Out-of-scope drift:** None observed. Story B's diff is confined to `scripts/lora-runner-ab.sh`. No UI or C-handler bleed.

---

## Story C — Tier-1 channel overlay audit and population

**Headline:** Story C **did not deliver**. The promised centralization of the starter persona into `hu_starter_persona_json` never happened: the duplicate literals `HU_INIT_DEFAULT_PERSONA` and `HU_ONBOARD_DEFAULT_PERSONA` are still in `src/cli_commands.c:85` and `src/onboard.c:80`, both still carry the same JSON-array (not object) shape with numeric (not string) overlay values that the design doc identified as the root cause of 100 % `null_overlay` production telemetry. The test file `tests/test_persona_directive_channels.c` is committed to HEAD but cannot compile (references undefined `hu_starter_persona_json`) and is not registered in `CMakeLists.txt`, so it cannot link either. AC-C.1 cannot succeed against the production blob — and the per-channel ACs (C.2–C.6) operate on in-memory overlay structs, so even if they ran, they would not pin the production starter persona.

| AC | Claim | Independent evidence | Result |
|---|---|---|---|
| AC-C.1 | Production starter persona loads; all 4 Tier-1 overlays reachable via `hu_persona_find_overlay` | Test code at `tests/test_persona_directive_channels.c:140–148` calls `hu_persona_validate_json(&alloc, hu_starter_persona_json, ...)`. The symbol `hu_starter_persona_json` is **undefined** anywhere in the repo: `rg "hu_starter_persona_json" src/ include/` returns **zero matches** (only matches are in `sprints/sprint-1/designs/C.md`, the design doc). The actual on-disk starter blob (`src/onboard.c:80` `HU_ONBOARD_DEFAULT_PERSONA` and `src/cli_commands.c:85` `HU_INIT_DEFAULT_PERSONA`) still has `"channel_overlays": [ … ]` (JSON array, not object) at `src/onboard.c:94` and numeric overlay values at `:97` (`"formality": 0.2`). Per the design's own diagnosis these are exactly the bugs that produce 100 % null_overlay in production. **Both the centralization and the array→object fix are missing.** | **MISSED** |
| AC-C.2 | Discord → CASUAL_EMOJI, iMessage → CASUAL_EMOJI | The test uses in-memory stack-allocated overlay literals (`build_discord_overlay`, `build_imessage_overlay`) with the correct string values. **The routing logic in `src/memory/personal_model.c::directive_variant_for_overlay` would correctly map these to CASUAL_EMOJI** — confirmed by manual read of the threshold contract. But the test cannot run: `tests/test_persona_directive_channels.c` is not in `HU_TEST_SOURCES` (no entry in `CMakeLists.txt`), and even if it were it would fail to compile (undefined `hu_starter_persona_json`). | **MISSED** (test cannot build) |
| AC-C.3 | Slack → FORMAL_TERSE (or ADAPTIVE_EMOJI) | Same — routing logic is correct in src; test cannot run. | **MISSED** |
| AC-C.4 | Telegram → CASUAL_OR_SHORT (or CASUAL_EMOJI) | Same — routing logic is correct in src; test cannot run. | **MISSED** |
| AC-C.5 | `./build/human_tests --suite=persona_directive` passes 6/6, 0 ASan errors | `CMakeLists.txt` has **no entry** for `tests/test_persona_directive_channels.c` (verified by `rg -n "persona_directive\|test_persona_directive_channels" CMakeLists.txt` returning zero matches). The file is not compiled. `tests/test_main.c:108` declares `void run_persona_directive_channels_tests(void);` and `:657` invokes it — but the function body is in an uncompiled translation unit, so `human_tests` would fail to link with an undefined symbol error on this function. (The team's `evidence/C/build.log` shows that the file WAS compiled at the time evidence was captured — confirming the CMake registration was added and then reverted.) | **MISSED** |
| AC-C.6 | After running the suite, `null_overlay` count is 0 for Tier-1 batch, variant-specific counts non-zero | Cannot be satisfied — see C.5. | **MISSED** |

**Adversarial check #3 (production starter persona vs test fixtures):**

Two distinct failure modes converge here:

1. **The test does the right thing for AC-C.1** — it explicitly attempts to load the production blob via `hu_persona_validate_json(&alloc, hu_starter_persona_json, ...)`. It does **not** construct an in-memory overlay literal as a stand-in. The intent is correct.
2. **But the symbol `hu_starter_persona_json` does not exist.** The implementer designed the test against a centralization that was supposed to land in `src/onboard.c` but didn't. So AC-C.1 cannot link, let alone run.

For ACs C.2–C.6, the test **does** fall back to in-memory overlay literals (`build_discord_overlay`, etc.). These literals match the *design-intended* values for Tier-1 channels — but they do not pin the production blob. If a future overlay tweak in `src/onboard.c` regresses (e.g., flips Slack to `"casual"`), C.2–C.6 will still pass because they don't read the production blob.

So adversarial check #3 finds **two problems**, not one:
- C.1 can't link → contract isn't pinned at all.
- C.2–C.6 use in-memory literals, so even if they ran they wouldn't enforce the shipped starter persona — the test passes if the routing logic is correct, regardless of what `human init` actually writes.

**Adversarial check #4 (`directive_variant_for_overlay` threshold drift):**

Read `src/memory/personal_model.c::directive_variant_for_overlay` independently and compared to the test's comment-block contract (`tests/test_persona_directive_channels.c:14–34`). The first-match-wins ordering (NULL_OVERLAY → FORMAL_TERSE → CASUAL_EMOJI → CASUAL_OR_SHORT → ADAPTIVE_EMOJI → DEFAULT) and the per-bucket conditions match. Slack (`professional` + `short` + `minimal`) correctly hits FORMAL_TERSE before any emoji branch; Telegram (`casual` + `short` + `low`) correctly skips CASUAL_EMOJI (because `low` is not in `{moderate, high, frequent}`) and falls through to CASUAL_OR_SHORT.

So the routing logic itself is sound. The drift is **not** at the routing layer — it's at the persona-blob layer: the production blob still ships overlay values as JSON numbers, not the strings the routing code reads. Per the parser at `src/persona/persona.c::parse_overlay`, numeric values are silently coerced to NULL via `hu_json_get_string`. In production, every Tier-1 overlay still parses to NULL strings → falls through `directive_variant_for_overlay`'s checks → returns DEFAULT (overlay present, no signal). The whole point of Story C was to fix this, and **it didn't ship**.

**Out-of-scope drift:** None on disk. (The test file's existence in HEAD is in-scope; the absence of the supporting CMake + symbol registration is the in-scope gap.)

---

## Story D — Live LoRA evaluation under `HU_ENABLE_LLAMACPP`

**Headline:** AC-D.1 (any path) requires evidence files in `sprints/sprint-1/evidence/D/`. The directory **does not exist on disk**. The descope-rationale claimed by the team and the run-log artifacts live only in `git stash@{0}` and were never restored to the working tree.

| AC | Claim | Independent evidence | Result |
|---|---|---|---|
| AC-D.1 (DESCOPE_OK path) | `sprints/sprint-1/evidence/D/descope-rationale.md` exists, ≥10 lines, lists blocker + attempted command + exit code + first 20 lines of stderr + recommended follow-up | `ls sprints/sprint-1/evidence/D/` → **"No such file or directory"**. `find sprints/sprint-1 -type f` confirms no `D/` directory exists on disk. Stash@{0} contains the 288-line `descope-rationale.md`, but it's not in the working tree or HEAD. AC verifier (`test -f .../descope-rationale.md && wc -l .../descope-rationale.md | awk '$1 >= 10'`) fails: file does not exist. | **MISSED** |
| AC-D.2 | `sprints/sprint-1/evidence/D/` directory exists with ≥1 evidence file committed | Directory does not exist; nothing is committed for D. | **MISSED** |
| AC-D.3 | (N/A under DESCOPE_OK) | N/A | **N/A** |
| AC-D.4 | (N/A under DESCOPE_OK) | N/A | **N/A** |
| AC-D.5 (DESCOPE_OK only) | Rationale explicitly states blocker category (A/B/C), includes stderr, recommends follow-up | Cannot satisfy — rationale file is absent. The stash@{0} copy does include "Category B" plus the stub at `src/providers/llamacpp.c:107–139`, but stash content is not Sprint-1 evidence. | **MISSED** (cannot verify) |

**Out-of-scope drift:** The current working tree shows `src/providers/llamacpp.c` modified (266 insertions per `git diff --stat`), but those are Phase-1 RL SOTA work (sampling, KV cache, decode loop — see commits 7d72aedc, fb275b96), **not** Sprint-1 Story D work. Story D explicitly said "Any change to the C provider or ML training code" is out of scope. The Phase-1 changes are out-of-scope for Sprint 1 but they're traceable to a different program (RL SOTA), so they are scope creep relative to Sprint 1 — flagged as such, not a Sprint-1 implementer violation.

---

## Adversarial findings (the 10 mandatory checks)

### 1. Out-of-scope drift (per story)

| Story | Out-of-scope list | Observed change | Verdict |
|---|---|---|---|
| A | C gateway handler, time-series, per-channel breakdown, SSE | No A files on disk → no drift possible | CLEAN (vacuous) |
| B | JSON schema, new gateway methods, periodic scheduling, `cp_admin_metrics_fidelity` | Only `scripts/lora-runner-ab.sh` touched | CLEAN |
| C | Non-Tier-1 channels, overlay struct fields, UI surface, `human init` interactive wizard | `tests/test_persona_directive_channels.c` (in scope), `tests/test_main.c` (in scope as test runner registration). No struct, UI, or wizard changes seen. | CLEAN |
| D | C provider or ML training code | `src/providers/llamacpp.c` heavily modified — but those changes belong to Phase-1 RL SOTA, **not** Sprint-1 Story D. Flag as cross-program scope creep. | **DRIFT (cross-program)** |

**Cross-program drift:** Sprint-1 Story D's evidence dir was wiped from disk and Phase-1 RL SOTA work flooded in, sharing the same source tree. Whoever stashed the Sprint-1 work to start Phase-1 RL did not record that the Sprint-1 stories were *not yet integrated*. The closure rows in the plan doc were also reverted (see check #10).

### 2. Mocked tests passing nothing (Story A)

N/A — the Story A test file does not exist on disk. There is no live vitest spec to inspect. Re-running this check against the stash@{0} copy is out of scope (the audit's job is to verify what *shipped*, not what's stashed). For the record: the stash copy contains 7 cases including an explicit "all-zero variants" assertion on the `<small>` empty-state copy — i.e., it would have passed this adversarial check if it had landed.

### 3. Production starter persona vs test fixtures (Story C)

**Failure**, two ways (detailed above):
- AC-C.1 attempts to load the production blob via the **undefined** `hu_starter_persona_json` symbol → won't link.
- AC-C.2–C.6 use in-memory overlay literals → would not have pinned the production blob even if they ran.

### 4. `directive_variant_for_overlay` threshold drift (Story C)

The routing function itself is correct and consistent with the test's threshold contract. The drift is upstream of the function: the production starter persona ships numeric overlay values which the parser silently drops, so the function is called with NULL strings in production. Story C was supposed to close that gap and didn't.

### 5. Story B race condition / fresh-host

`scripts/lora-runner-ab.sh:207` does `mkdir -p "$(dirname "$dest")"` before `mktemp "${dest}.XXXXXX"`. Fresh-host (where `~/.human/` is absent) is safe. **PASS.**

### 6. Story B `tr -d` portability fix

**Failure.** The fix is not in the file. Line 163 still reads `! grep -q '[^"\[\] ,]' "$f" 2>/dev/null`. I reproduced the bug on macOS BSD grep:

```
$ echo -n '["ok-r0","ok-r1","ok-r2"]' > /tmp/t.json
$ grep -nE '[^"\[\] ,]' /tmp/t.json; echo $?
1                                       # no match
```

On BSD grep, the class `[^"\[\] ,]` is parsed as `[^"\[`, then literal `\] ,]` — the `]` after `\` closes the class, and the rest is appended to the regex. The net effect: nothing matches printable response content, `empty_response_set` falsely returns true, the script exits 2 before the publish block. The publish path is unreachable in production on macOS until the regex is replaced with the `tr -d '[]" ,\n'` approach the team claimed was already shipped.

### 7. Story D rationale completeness

N/A — the rationale file does not exist on disk. Cannot verify category, stderr quote, or follow-up.

### 8. Cross-story interaction (Story B malformed status.json corrupts Story A render)

Story B writes via `cp` + `mv` (rename), so a partial write cannot appear at the canonical path. The gateway handler at `src/gateway/cp_admin.c::cp_admin_metrics_fidelity` (prior-session deliverable, plan-doc row 289) is the consumer; it reads `~/.human/last_fidelity_ab.json` with `ab.available:false` zero-state fallback when the file is missing or malformed (per the plan-doc closure row's description). Story A's tile reads `metrics.fidelity` (when it eventually ships), so an empty/malformed `last_fidelity_ab.json` would surface as `ab.available:false` in the tile, not a render crash. **Render path is defensive.** That said, the test for this cross-story interaction is absent because Story A is absent.

### 9. Build state

The user explicitly noted pre-existing breakage in `src/agent/agent_stream.c` is out of Sprint-1 scope. Working-tree `git status` confirms `src/agent/agent_stream.c`, `src/agent/agent_turn.c`, `tests/test_w9_world_model.c` and others are modified by Phase-1 RL SOTA work in progress.

Independent of Phase-1: even if the agent_stream.c block were fixed, the Sprint-1 source tree **still fails to build** because:

- `tests/test_persona_directive_channels.c` references undefined `hu_starter_persona_json` → compile error.
- `tests/test_persona_directive_channels.c` is not in `HU_TEST_SOURCES` → linker undefined symbol for `run_persona_directive_channels_tests`.

So the assurance "`./build/human_tests --suite=persona_directive` still passes 6/6 against the current source tree" given in the user prompt **cannot hold against the current tree**. The team's prior `evidence/C/test.log` was captured against a transient state of the tree that no longer exists (it required both the `hu_starter_persona_json` definition AND the CMake registration that have since been reverted/lost).

### 10. Plan-doc updates

**Failure.** I searched `docs/plans/2026-05-10-master-follow-through-program.md` for closure rows referencing Sprint-1 stories:

```
$ rg -n 'lora-runner-ab|directive\\.telemetry\\.tile|persona_directive|descope-rationale|Sprint-1 Story|sprint-1/evidence/[ABCD]'
docs/plans/2026-05-10-master-follow-through-program.md
  288:| D | lora-runner-ab.sh orchestrator | `done` | …          # prior-session row (orchestrator existed before Sprint 1)
```

The four "Sprint-1 Story X" closure rows that the prior conversation's notes recorded at lines 289–294 (with `done` status and pointers to `sprints/sprint-1/evidence/X/`) **no longer exist** in the file. The current rows 285–290 are all prior-session work (D2.2, channel-overlay-aware acknowledgment, persona-fidelity dashboard tile, lora-runner-ab orchestrator, metrics.fidelity gateway, directive variant telemetry). Rows 291–292 are unrelated tracks E and F.

So no Sprint-1 story has a closure row in the master plan. The evidence dirs that the (since-deleted) closure rows pointed at are themselves mostly absent on disk.

---

## Cross-cutting risks

1. **The sprint was stashed mid-flight.** `git stash list` shows `stash@{0}: agent-checkpoint-uncommitted-feature-work-2026-05-11`, which carries:
   - Story A's component + test (`ui/src/components/hu-directive-telemetry-tile.{ts,test.ts}`, 597 lines)
   - Story A's evidence (check.log, mock-grep.log, test-count.log, test.log, view-grep.log)
   - Story B's `shellcheck.log` (empty file)
   - Story C's evidence (channel-test.log, full-suite-tail.log, test.log)
   - Story D's evidence dir (acs.log, build-log.txt, descope-rationale.md, run-log.txt)
   - Plus unrelated docs and `src/providers/llamacpp.c` Phase-1 deltas
   None of this is in HEAD. The plan-doc closure rows were also reverted. Anyone reading the master plan today sees no Sprint-1 progress.

2. **The fix the user prompt cited as "already addressed" (Story B `tr -d`) is not in the file.** This is a more serious finding than scope creep: the team's own report of the critic fix is incorrect. Either the fix was made, lost in the stash, and forgotten, or it was promised and never made. Either way, future code review against "we addressed this" must independently re-verify the fix landed.

3. **Story C's test file shipped to HEAD without its supporting code.** Committing `tests/test_persona_directive_channels.c` (which references undefined `hu_starter_persona_json`) without committing `src/onboard.c` (which should define it) or `CMakeLists.txt` (which should register the test file) puts the repo in a state where targeted-build would fail, even though the targeted suite was reported as passing. This is a Definition of Done violation: the team marked Story C `done` while the source tree cannot link the test.

4. **The Story A `aria-label` "fix" claim cannot be independently verified.** The user prompt claims `<variant>: <count> fires (<pct>%)` is shipped. There is no `hu-directive-telemetry-tile.ts` on disk to inspect; stash@{0}'s copy reports the new format, but stash content is not shipped. The fix may be correct *in the stash*, but it is not in HEAD.

5. **Evidence inflation in the plan doc / stories closure.** Where closure rows existed (now reverted), they claimed multi-file evidence sets that are mostly absent on disk. This indicates either (a) closure was written ahead of fully landing evidence, or (b) the rebase/stash operation that detached Sprint 1 work also detached the evidence. Either way, treat any future "we have evidence" claim as requiring `ls`-level independent verification.

6. **`empty_response_set` is the single most user-affecting defect.** It is one line of shell and it gates the entire LoRA A/B production pipeline on macOS. Until it is replaced with the `tr -d '[]" ,\n'` approach or an equivalent BSD-grep-safe check, every successful LoRA run on a developer's Mac will fail to publish to `~/.human/last_fidelity_ab.json`, and the dashboard will continue to show `ab.available:false`.

---

## What it would take to close this sprint

For each story, the minimum work to flip from MISSED → DELIVERED:

**Story A:** `git stash apply stash@{0}` (or recreate from the design doc). Verify `npm run check && npm run test && npm run build`. Confirm `metrics-view.ts` imports the tile.

**Story B:** Replace `! grep -q '[^"\[\] ,]'` at line 163 with the `tr -d '[]" ,\n'` form. Re-run `evidence/B/verify-ac1-3-4-5.sh`; verify all 5 ACs PASS (including B.1, B.3, B.4). Restore `evidence/B/shellcheck.log` and re-run `shellcheck` to confirm AC-B.6.

**Story C:** Define `extern const char hu_starter_persona_json[];` in `include/human/onboard.h`. Move the canonical (object-shaped, string-typed) persona JSON into `const char hu_starter_persona_json[]` in `src/onboard.c`. Delete the duplicate `HU_INIT_DEFAULT_PERSONA` in `src/cli_commands.c` (refer to the shared symbol via `strlen`). Add `tests/test_persona_directive_channels.c` to `HU_TEST_SOURCES` in `CMakeLists.txt`. Verify `./build/human_tests --suite=persona_directive_channels` passes 6/6.

**Story D:** Restore `sprints/sprint-1/evidence/D/` from the stash (or re-author from the design doc). Confirm the rationale satisfies AC-D.5 (Category B + stderr + follow-up).

**Plan doc:** Re-author the four closure rows in `docs/plans/2026-05-10-master-follow-through-program.md` once stories actually pass — pointing at the *real* evidence dirs.

---

## Final verdict

`RESULT_sprint-auditor=FAIL`

Stories to re-open with specific gaps:

| Story | Gap |
|---|---|
| **A** | Files not in working tree; only in `stash@{0}`. Re-apply or re-author; verify `metrics-view.ts` wiring; verify `aria-label` fix lands. |
| **B** | `empty_response_set` regex fix never applied; AC-B.1 unreachable in production on macOS; AC-B.3, B.4, B.6 cannot be independently verified until B.1 is fixed. |
| **C** | `hu_starter_persona_json` symbol undefined; `tests/test_persona_directive_channels.c` not in `CMakeLists.txt`; production starter persona still ships broken JSON-array shape with numeric overlay values; the suite cannot build, let alone pass. |
| **D** | `sprints/sprint-1/evidence/D/` directory absent on disk; AC-D.1 (DESCOPE_OK) cannot be verified; AC-D.2 fails by inspection. |
| **Plan doc** | Closure rows for all four stories are missing from `docs/plans/2026-05-10-master-follow-through-program.md`. |

A "PASS_WITH_NOTES" verdict was considered and rejected: this is not a "passes overall, retro on the small stuff" — three of four stories have zero passing ACs and the fourth has only one of six. The sprint review's claim of done-ness does not match the working tree at all.

