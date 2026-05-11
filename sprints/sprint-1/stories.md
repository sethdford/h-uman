---
title: "Sprint 1 — SOTA persona-fidelity follow-through"
created: 2026-05-11
status: done
sprint: 1
program: docs/plans/2026-05-10-master-follow-through-program.md
---

# Sprint 1 — SOTA persona-fidelity follow-through

## Sprint goal

Close four half-open loops left after shipping the live `metrics.fidelity` gateway method and acknowledgment-directive variant telemetry: surface variant distribution in the UI (Story A), make the LoRA orchestrator write to the canonical path the dashboard reads (Story B), populate Tier-1 channel overlays so directive variants actually fire per channel (Story C), and either prove or explicitly descope the live LoRA evaluation path (Story D).

---

## Story A — Directive telemetry dashboard tile

**As a** human operator reviewing persona-fidelity health
**I want** a visual breakdown of which acknowledgment-directive variants fired across all channels (stacked bar or segmented row, plus total count) embedded in the Metrics view
**So that** I can see at a glance whether `null_overlay` / `default` dominate (a signal that channel overlays are missing) or whether `formal_terse` / `casual_emoji` / etc. are lighting up as intended

**Acceptance Criteria** (every AC must be a single PASS/FAIL evidence claim):

- [x] AC-A.1: `ui/src/components/hu-directive-telemetry-tile.ts` exists, exports a `@customElement("hu-directive-telemetry-tile")` LitElement, and `npm run build` completes with zero TypeScript errors.
- [x] AC-A.2: The component fetches `metrics.directive_telemetry` from the gateway (or demo-gateway), renders one labelled segment per variant key (`null_overlay`, `default`, `formal_terse`, `casual_emoji`, `casual_or_short`, `adaptive_emoji`), and exposes a `total` count — verified by running `npm run test` against the component's vitest spec and observing all assertions green.
- [x] AC-A.3: When the gateway call is in-flight the component renders a `<hu-skeleton>` loading state; when the call rejects with an error it renders an inline error banner (not a blank div) — verified by vitest using a stub gateway that resolves on a timer then rejects.
- [x] AC-A.4: `ui/src/views/metrics-view.ts` imports and renders `<hu-directive-telemetry-tile>` in at least one non-stub view path — verified by `rg "hu-directive-telemetry-tile" ui/src/views/metrics-view.ts` returning at least one match.
- [x] AC-A.5: `ui/src/demo-gateway.ts` contains a `metrics.directive_telemetry` mock whose `variants` object has all six variant keys and whose `total` equals their sum — verified by `rg "directive_telemetry" ui/src/demo-gateway.ts` returning a match and `npm run test` passing.
- [x] AC-A.6: `npm run check` (which includes `npm run lint:tokens`) exits 0 with no raw hex, `rgba()`, hardcoded durations, or missing `--hu-*` token violations in the new component file.
- [x] AC-A.7: At least 4 vitest test cases exist in the component's test file covering: render with populated data, render with all-zero variants, loading skeleton state, and error state — verified by `rg "describe\|it\(" ui/src/components/hu-directive-telemetry-tile.test.ts | wc -l` printing ≥ 4.

**Out of scope:**
- Historical time-series of variant distribution (no chart over time, just current snapshot)
- Per-channel variant breakdown (channel drill-down is a future enhancement)
- Server-Sent Event live push of telemetry (polling or one-shot fetch only)
- Any change to the C gateway handler `cp_admin_metrics_directive_telemetry` itself

**Dependencies:** `metrics.directive_telemetry` C handler and demo-gateway mock must exist before AC-A.2 can pass in integration mode (the vitest spec may stub the gateway)
**Estimated risk:** low — pattern is identical to existing `hu-stat-card` / `hu-stats-row` wiring in `metrics-view.ts`; only the stacked-bar layout is novel

---

## Story B — Orchestrator writes canonical A/B status path

**As a** developer running the LoRA A/B evaluation pipeline
**I want** `scripts/lora-runner-ab.sh` to atomically publish its `status.json` output to `~/.human/last_fidelity_ab.json` (or `$HUMAN_FIDELITY_AB_PATH` when set) at the end of a successful run
**So that** the `cp_admin_metrics_fidelity` gateway handler — which reads exactly that path — can surface real A/B results in the Metrics view without manual copy-paste

**Acceptance Criteria** (every AC must be a single PASS/FAIL evidence claim):

- [x] AC-B.1: After a successful invocation of `scripts/lora-runner-ab.sh` (or its no-provider stub path that exits 0), `~/.human/last_fidelity_ab.json` exists and its contents equal the `status.json` produced in `--output-dir` — verified by `diff ~/.human/last_fidelity_ab.json <output-dir>/status.json` printing nothing.
- [x] AC-B.2: The write is atomic (tmp-file + rename): at no point does a partial write appear at the canonical path — verified by `grep -n "mv\s" scripts/lora-runner-ab.sh` showing the rename command rather than a direct redirect to `~/.human/last_fidelity_ab.json`.
- [x] AC-B.3: When `--no-publish` is passed, `~/.human/last_fidelity_ab.json` is NOT created or modified — verified by running the script with `--no-publish` in a clean tempdir and then `test ! -f ~/.human/last_fidelity_ab.json` (or diff confirms no change if file pre-existed).
- [x] AC-B.4: When `HUMAN_FIDELITY_AB_PATH=/tmp/test-ab.json` is exported, the script writes to `/tmp/test-ab.json` instead of `~/.human/last_fidelity_ab.json` — verified by `test -f /tmp/test-ab.json && test ! -newer ~/.human/last_fidelity_ab.json /tmp/test-ab.json` (or by checking only `/tmp/test-ab.json` was written).
- [x] AC-B.5: A run that exits non-zero (e.g., the no-provider path that exits 2) does NOT write or overwrite the canonical file — verified by running the no-provider stub path and confirming `~/.human/last_fidelity_ab.json` is unchanged (or absent).
- [x] AC-B.6: `shellcheck scripts/lora-runner-ab.sh` exits 0 with no warnings or errors (SC2034, SC2086, etc. all clean).

**Out of scope:**
- Changing the JSON schema of `status.json` / `last_fidelity_ab.json`
- Adding any new gateway methods
- Automatic periodic scheduling or cron wiring
- Any change to the C handler `cp_admin_metrics_fidelity`

**Dependencies:** `scripts/lora-runner-ab.sh` must already exist with a stub exit-2 no-provider code path (prior session deliverable); Story D may exercise the same script
**Estimated risk:** low — pure shell change; atomicity pattern (tmp+mv) is idiomatic and well-precedented in the repo

---

## Story C — Tier-1 channel overlay audit and population

**As a** persona system designer
**I want** each Tier-1 channel (Telegram, Discord, iMessage, Slack) to carry meaningful `formality`, `avg_length`, and `emoji_usage` values in its persona overlay
**So that** `acknowledgment_directive_for_overlay` returns the correct variant for each channel, and `directive_telemetry` snapshots taken after a synthetic run show non-zero counts in at least `casual_emoji` (Discord/iMessage), `formal_terse` or `adaptive_emoji` (Slack), and `casual_or_short` or `casual_emoji` (Telegram) — proving the overlay-to-variant routing is live end-to-end

**Acceptance Criteria** (every AC must be a single PASS/FAIL evidence claim):

- [x] AC-C.1: Each of the four Tier-1 channels has a non-empty overlay entry reachable via `hu_persona_find_overlay(persona, "<channel_id>")` — verified by a C test that constructs or loads the starter persona and asserts `overlay != NULL` for `"telegram"`, `"discord"`, `"imessage"`, and `"slack"`.
- [x] AC-C.2: `directive_variant_for_overlay` (or `acknowledgment_directive_for_overlay`) returns `CASUAL_EMOJI` for the Discord overlay and `CASUAL_EMOJI` (or `CASUAL_OR_SHORT`) for the iMessage overlay — verified by a C test with hard assertions on the return value.
- [x] AC-C.3: `directive_variant_for_overlay` returns `FORMAL_TERSE` or `ADAPTIVE_EMOJI` for the Slack overlay (professional + minimal-emoji → terse or adaptive path) — verified by a C test with hard assertion on the return value.
- [x] AC-C.4: `directive_variant_for_overlay` returns `CASUAL_OR_SHORT` or `CASUAL_EMOJI` for the Telegram overlay — verified by a C test with hard assertion on the return value.
- [x] AC-C.5: A synthetic test (`tests/test_persona_directive_channels.c` or equivalent) that runs `acknowledgment_directive_for_overlay` for all four Tier-1 channels in a loop passes with `cmake --build build && ./build/human_tests --suite=persona_directive` showing 0 failures, 0 ASan errors.
- [x] AC-C.6: After `./build/human_tests --suite=persona_directive`, calling `directive_telemetry_snapshot()` (or equivalent telemetry accumulator) returns a snapshot where the `null_overlay` count is 0 for those four channels and the variant-specific counts are non-zero — verified by a test assertion or by inspecting the snapshot struct in the test.

**Out of scope:**
- Overlays for any channel outside the four Tier-1 channels (Tier-2/3 channels are untouched this sprint)
- Changing the `hu_persona_overlay_t` struct definition or adding new overlay fields
- UI changes to surface per-channel variant breakdown (that is Story A's follow-on)
- Modifying `human init` or the onboarding wizard to generate overlays interactively

**Dependencies:** `acknowledgment_directive_for_overlay` / `directive_variant_for_overlay` C function must exist from the prior session; `hu_persona_overlay_t.formality` / `.avg_length` / `.emoji_usage` are confirmed present in `include/human/persona.h`
**Estimated risk:** medium — the variant routing logic must correctly map the overlay field values to the expected enum; if the routing thresholds are not yet documented, they must be discovered by reading the implementation before writing the test assertions

---

## Story D — Live LoRA evaluation under `HU_ENABLE_LLAMACPP`

**As a** ML engineer validating the M3 private-learning pipeline
**I want** evidence that `scripts/lora-runner-ab.sh` can run end-to-end with a GGUF model + LoRA adapter (real or synthetic), produce a valid `status.json` with a non-zero `delta` against `tests/fixtures/lora_baseline_persona.json`, and emit that file without error
**So that** Track D of the master follow-through program can move from `in_progress` to `done` (or be explicitly descoped with a precise blocker recorded)

**Acceptance Criteria** (every AC must be a single PASS/FAIL evidence claim):

- [ ] AC-D.1 *(path a — real GGUF)*: `tests/fixtures/lora_baseline_persona.json` exists and `sprints/sprint-1/evidence/D/run-log.txt` shows a completed orchestrator run with exit code 0, a `status.json` at the configured output path, and `delta > 0.0` in that JSON — verified by `jq '.delta > 0' sprints/sprint-1/evidence/D/status.json` printing `true`.
  **OR**
- [ ] AC-D.1 *(path b — synthetic GGUF)*: `tests/fixtures/synthetic_lora.gguf` exists (≥ 1 byte, deterministic synthetic content), `scripts/lora-runner-ab.sh` runs to completion against it, and `sprints/sprint-1/evidence/D/status.json` contains `"delta": <value> > 0` — verified by `jq '.delta > 0' sprints/sprint-1/evidence/D/status.json` printing `true`.
  **OR**
- [x] AC-D.1 *(DESCOPE_OK)*: `sprints/sprint-1/evidence/D/descope-rationale.md` exists and contains: (1) the precise error or missing prerequisite that blocks a local run, (2) the exact command that was attempted, (3) the exit code and first 20 lines of stderr, and (4) a recommended follow-up action — verified by `test -f sprints/sprint-1/evidence/D/descope-rationale.md && wc -l sprints/sprint-1/evidence/D/descope-rationale.md | awk '$1 >= 10'` printing the line count (≥ 10 lines).

- [x] AC-D.2: Whichever path is taken, a `sprints/sprint-1/evidence/D/` directory exists with at least one evidence file (`run-log.txt`, `status.json`, or `descope-rationale.md`) committed to the repo — verified by `ls sprints/sprint-1/evidence/D/` listing at least one file.
- [ ] AC-D.3: If path (a) or (b) is taken, `cmake --build build -DHU_ENABLE_LLAMACPP=ON` compiles cleanly (zero errors) and `./build/human_tests --suite=ml` shows 0 failures — verified by the CI log or a local run log in `sprints/sprint-1/evidence/D/build-log.txt`.
- [ ] AC-D.4: The `status.json` produced (paths a or b) conforms to the schema expected by `cp_admin_metrics_fidelity` — specifically it contains at minimum the keys `delta`, `baseline_score`, `candidate_score`, and `run_id` — verified by `jq 'has("delta") and has("baseline_score") and has("candidate_score") and has("run_id")' sprints/sprint-1/evidence/D/status.json` printing `true`.
- [x] AC-D.5 *(DESCOPE_OK only)*: The descope rationale explicitly states which of the three blocker categories applies: (A) no GGUF model available locally and download is blocked by network/licence policy, (B) `HU_ENABLE_LLAMACPP` CMake flag is not yet wired to a real llama.cpp backend, or (C) the `lora-runner-ab.sh` script does not yet exist — verified by `grep -E "blocker category|Category [A-C]" sprints/sprint-1/evidence/D/descope-rationale.md` returning a match.

**Out of scope:**
- Training a new LoRA adapter from scratch (the story consumes an existing or synthetic adapter, it does not train one)
- Integrating the adapter into the live chat path (that is Track D Phase D1 in the master program, not this story)
- Benchmarking inference latency or memory usage
- Any change to the C provider or ML training code

**Dependencies:** `tests/fixtures/lora_baseline_persona.json` (prior session deliverable); `HU_ENABLE_LLAMACPP` CMake flag (must exist or be documented as missing in the descope rationale)
**Estimated risk:** high — whether a runnable GGUF + adapter pair is available locally is an operational unknown; the DESCOPE_OK path is explicitly provided so this story does not block the sprint

---

## Non-goals (sprint-wide)

- We will NOT modify the C handler signatures for `cp_admin_metrics_directive_telemetry` or `cp_admin_metrics_fidelity`.
- We will NOT add new `hu_persona_overlay_t` struct fields (AC-C uses only the three existing fields).
- We will NOT build a time-series chart for directive telemetry (AC-A renders current snapshot only).
- We will NOT wire periodic cron execution of `lora-runner-ab.sh` (Story B is about the write path, not scheduling).
- We will NOT train a new base model or fine-tune beyond a fixture LoRA adapter (Story D).

## Open questions for stakeholder

- Story A: Should the stacked bar use `--hu-chart-categorical-*` tokens in variant-label order, or should the six variants map to specific semantic tokens (e.g., `null_overlay` → `--hu-error-dim`)? This affects the component's CSS but not its existence AC.
- Story C: The variant routing thresholds (e.g., what `formality` string value triggers `FORMAL_TERSE` vs `ADAPTIVE_EMOJI`) are implementation-defined — are they documented anywhere, or must the implementer read `src/persona/` source to discover them before writing AC-C.3?
- Story D: Is there an approved synthetic GGUF generator script or fixture-generation policy in the repo, or should the implementer create a minimal deterministic binary fixture from scratch?

---

`RESULT_product-owner=READY`
