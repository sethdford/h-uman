# iMessage Action Surface — Implementation Tasks

**Date:** 2026-05-25
**Spec dir:** `docs/plans/2026-05-25-imessage-action-surface/`
**Requirements:** [requirements.md](requirements.md)
**Design:** [design.md](design.md)
**Status:** Draft for review

Tasks are ordered by dependency. Each task lists the AC(s) it satisfies, the files it touches, the verification command, and an estimated size. Per `~/.claude/rules/agent-task-sizing.md`: no task exceeds ~8 mechanical sites; multi-file tasks have explicit per-file pointers.

## Phase A — Pure predicate + config (no AX, no I/O)

### T-A1: Add `hu_reply_style_facts_t`, enum, scoring fn, sampling fn
- **AC:** AC-1
- **Files:** `include/human/channels/imessage_action.h` (new), `src/channels/imessage_action.c` (new)
- **Touchpoints:** new module only, no vtable changes yet
- **Verify:** unit test stub compiles
- **Size:** ~150 LOC src + ~40 LOC header
- **Owner:** unassigned

### T-A2: Unit-test the predicate truth table (12 anchor cases)
- **AC:** AC-1
- **Files:** `tests/test_imessage_reply_style.c` (new), `tests/test_main.c` (wire `run_imessage_reply_style_tests`)
- **Cases to pin (one per row):**
  1. fresh (`sec=5`), low density, fresh msg → FLAT high prob
  2. stale (`sec=900`), 1 pending Q → THREADED high prob
  3. rapid-fire (`density=15`) → FLAT
  4. 3 other-threaded recent → THREAD nudge
  5. parent_was_a_question + persona_thread_affinity=0.6 → THREADED likely
  6. emotional_intensity=HIGH + density=2 → never TAPBACK solo (AC-3)
  7. emotional_intensity=HIGH → TAPBACK_PLUS_FLAT possible
  8. parent_position=10 + sec=300 → THREADED (scrolled off)
  9. persona_thread_affinity=0.05 → almost never THREADED
  10. persona_thread_affinity=0.9 → mostly THREADED
  11. mirror=0 + density=4 + sec=30 → FLAT
  12. formality=1.0 + sec=120 → THREAD nudged up
- **Verify:** `./build/human_tests --filter=reply_style` → 12/12 pass
- **Size:** ~200 LOC test
- **Depends on:** T-A1
- **Owner:** unassigned

### T-A3: Distribution shape test (100 synthetic facts)
- **AC:** AC-2
- **Files:** `tests/test_imessage_reply_style.c` (extend), `tests/fixtures/imessage_action/distribution_facts.json` (new)
- **Generator:** small Python or C script that emits 100 fact-tuples spanning the parameter space; checked in as JSON
- **Assertions:**
  - Global thread-rate ∈ [15%, 65%]
  - `other_threaded_recent >= 2` subset thread-rate ≥ 2× global
  - `density > 6` subset thread-rate ≤ 0.5× global
- **Verify:** `./build/human_tests --filter=style_distribution` → pass
- **Size:** ~150 LOC test + fixture
- **Depends on:** T-A2
- **Owner:** unassigned

### T-A4: Parametric emotional-protection test
- **AC:** AC-3
- **Files:** `tests/test_imessage_reply_style.c` (extend)
- **Sweep:** all 16 combinations of {density, formality, mirror, position} × intensity ≥ MEDIUM, with 100 RNG seeds each → 1600 invocations
- **Assert:** zero `HU_REPLY_STYLE_TAPBACK` solo returns
- **Verify:** `./build/human_tests --filter=emotional_protection` → pass
- **Size:** ~80 LOC test
- **Depends on:** T-A2
- **Owner:** unassigned

### T-A5: New config keys + parser + one-shot disabled-warn
- **AC:** supports AC-4, AC-7, AC-9 (config gate)
- **Files:** `src/config_parse.c` (add `iMessage.action_surface_v2.*`), `src/config.c` (defaults), `include/human/config.h` (struct fields)
- **Per [silent-config-gated-subsystems.md](../../../.claude/rules/silent-config-gated-subsystems.md):** emit one-shot disabled-warn log at first invocation
- **Verify:** new test `tests/test_config_action_surface.c` parses both defaults and overrides; logs assert
- **Size:** ~120 LOC across 3 files + ~80 LOC test
- **Depends on:** none
- **Owner:** unassigned

## Phase B — Vtable extension + 42-channel stubs

### T-B1: Extend `hu_channel_vtable_t` with reply/react_emoji/send_sticker
- **AC:** AC-4, AC-5, AC-6 (vtable surface)
- **Files:** `include/human/channel.h` (vtable + struct)
- **Stub:** all 42 other channel impls return `HU_ERR_NOT_SUPPORTED` for the 3 new slots
- **Per `~/.claude/rules/agent-task-sizing.md`:** 42 sites × 3 slots = 126 mechanical edits → use a script (`scripts/wire-channel-stubs.sh`), NOT an agent
- **Verify:** `cmake --build build` clean; `./build/human_tests` no regression
- **Size:** vtable change ~30 LOC, script ~50 LOC, mechanical stubs ~252 LOC across 42 files
- **Depends on:** none
- **Owner:** unassigned (script-run, then code review)

### T-B2: Wire telemetry JSONL helper
- **AC:** AC-8
- **Files:** `src/channels/imessage_action.c` (extend with `hu_imessage_action_log_jsonl`), `include/human/channels/imessage_action.h` (extend), `src/channels/imessage_action_log_path.c` (new — resolves `~/.human/logs/imessage_action.jsonl`)
- **Verify:** `tests/test_imessage_action_telemetry.c` writes 3 events, reads back the file, asserts 3 well-formed JSONL lines with all required keys
- **Size:** ~120 LOC src + ~80 LOC test
- **Depends on:** T-A1, T-A5
- **Owner:** unassigned

## Phase C — Reply path (the headline feature)

### T-C1: AX row-focus + Cmd-R (Tier 1)
- **AC:** AC-4 (tier 1)
- **Files:** `src/channels/imessage_reply.c` (new), uses existing `ax_open_conversation`, `ax_find_message_group`
- **Logic:** open conv → find row by parent guid (via chat.db lookup of guid → text prefix) → AX focus row → AXPostKeyboardKey ⌘+R → wait for composer text field (200ms poll, 1s budget) → type body → Return
- **Verify:** integration test against a live macOS Messages.app (manual; documented in test plan); CI uses AX mock harness
- **Size:** ~250 LOC src
- **Depends on:** T-B1
- **Owner:** unassigned

### T-C2: AX `AXShowMenu` → "Reply…" (Tier 2)
- **AC:** AC-4 (tier 2)
- **Files:** `src/channels/imessage_reply.c` (extend)
- **Logic:** mirror of `ax_perform_tapback_on_row` — `AXShowMenu` → iterate items → match title.startswith("Reply") → click → wait composer → type body → Return
- **Verify:** AX mock harness asserts the correct menu-item-title match including the Unicode-ellipsis variants
- **Size:** ~150 LOC src
- **Depends on:** T-C1
- **Owner:** unassigned

### T-C3: Flat-send fallback + warn log (Tier 3)
- **AC:** AC-4 (tier 3)
- **Files:** `src/channels/imessage_reply.c` (extend), reuses `vtable->send`
- **Logic:** call `imessage_send` with the body; log WARN with the reason for downgrade
- **Verify:** test that, given an AX harness that fails tier 1 AND tier 2, the message still sends and a WARN line lands
- **Size:** ~40 LOC src + ~60 LOC test
- **Depends on:** T-C2
- **Owner:** unassigned

### T-C4: Wire `reply()` into iMessage vtable
- **AC:** AC-4
- **Files:** `src/channels/imessage.c` (vtable initializer)
- **Verify:** `tests/test_imessage_threaded_reply.c` exercises the full 3-tier path with AX mock; integration test (manual) confirms `reply_to_guid` appears in chat.db on a real send
- **Size:** ~30 LOC src + ~250 LOC test
- **Depends on:** T-C3
- **Owner:** unassigned

### T-C5: Reply pacing — `hu_persona_pace_reply` with jitter
- **AC:** AC-7
- **Files:** `src/persona/pacing.c` (new or extend existing pacing module), `include/human/persona.h` (add `reply_delay_variance_ms`)
- **Logic:** see design §6; wrap `imessage_reply` entry/exit to enforce minimum elapsed time
- **Verify:** `tests/test_imessage_reply_pacing.c` — 20 iterations with stubbed AX (instant return); assert elapsed ≥ min_delay × 1.2
- **Size:** ~80 LOC src + ~80 LOC test
- **Depends on:** T-C4
- **Owner:** unassigned

## Phase D — Custom-emoji tapback

### T-D1: AX path to bottom-row emoji picker
- **AC:** AC-5
- **Files:** `src/channels/imessage_react.c` (extract custom-emoji path from `imessage.c` if it grows past 200 LOC; otherwise extend in place)
- **Logic:** AXShowMenu → navigate to the 6-emoji sub-row (Sonoma+) → match child by Unicode codepoint of AXValue → click
- **Verify:** AX mock harness with 6 emoji children
- **Size:** ~180 LOC src
- **Depends on:** T-B1

### T-D2: Classic fallback map + `react_emoji` vtable wire
- **AC:** AC-5
- **Files:** `src/channels/imessage_react.c` (extend), `src/channels/imessage.c` (vtable initializer)
- **Logic:** `CLASSIC_MAP` lookup → if hit, call existing `ax_tapback` with the classic label; else attempt T-D1 sub-picker; else `HU_ERR_NOT_SUPPORTED`
- **Verify:** `tests/test_imessage_custom_tapback.c` covers 6 sub-picker emojis + 4 classic-fallback emojis + 1 unsupported emoji → `HU_ERR_NOT_SUPPORTED`
- **Size:** ~80 LOC src + ~200 LOC test
- **Depends on:** T-D1

## Phase E — Sticker MVP

### T-E1: Persona sticker picker
- **AC:** AC-6 (picker)
- **Files:** `src/persona/sticker.c` (new), `include/human/persona/sticker.h` (new)
- **Logic:** scan `~/.human/stickers/` for files matching `<context>-<mood>_<seq>.{png,heic}` → filter by inferred conv context-mood → uniform sample weighted by least-recently-used (recency tracked in `~/.human/state/sticker_lru.txt`)
- **Verify:** `tests/test_persona_sticker.c` constructs a tmp dir with 5 tagged stickers, asserts correct selection for {casual, happy}, {formal, acknowledgment}, etc.
- **Size:** ~180 LOC src + ~150 LOC test

### T-E2: Wire `send_sticker()` into iMessage vtable
- **AC:** AC-6 (send)
- **Files:** `src/channels/imessage_sticker.c` (new — wraps existing `imsg send --file` path), `src/channels/imessage.c` (vtable initializer)
- **Verify:** `tests/test_imessage_sticker.c` mock the `imsg send` path, assert `--file <path>` is in argv
- **Size:** ~80 LOC src + ~100 LOC test
- **Depends on:** T-B1, T-E1

### T-E3: User-facing docs for sticker dir + tag schema
- **AC:** AC-6 (UX)
- **Files:** `~/.human/stickers/README.md` (template), `docs/guides/imessage-stickers.md` (new), README link
- **Verify:** docs render in marketing site build (`pnpm --filter website build`)
- **Size:** ~100 lines of markdown

## Phase F — Style dispatcher wiring

### T-F1: Reply-style dispatcher at daemon-level
- **AC:** AC-1 → AC-8 (integration)
- **Files:** `src/daemon.c` (extend reply-construction code path), `src/channels/imessage.c` (use the predicate output to choose `reply()` vs `send()` vs `react_emoji()`)
- **Logic:** when daemon is about to reply on iMessage:
  1. build `hu_reply_style_facts_t` from current conv state + persona
  2. call `hu_imessage_choose_reply_style(facts, arc4random_u64())`
  3. switch on result → `vtable->reply` / `vtable->send` / `vtable->react_emoji` / both
  4. emit telemetry JSONL per AC-8
- **Verify:** `tests/test_imessage_dispatcher.c` exercises all 4 style branches with a mock chat.db + mock vtable
- **Size:** ~150 LOC dispatcher + ~250 LOC test
- **Depends on:** T-C5, T-D2, T-E2, T-B2

### T-F2: Build facts from real conv state
- **AC:** AC-1 → AC-8 (real data)
- **Files:** `src/channels/imessage_action_facts.c` (new), `include/human/channels/imessage_action.h` (extend)
- **Logic:** query last 20 inbound + 20 outbound messages from chat.db → compute density (msgs per minute over last 5 min) → count pending Qs → count their/our recent threaded replies → look up parent's text prefix + position
- **Verify:** `tests/test_imessage_action_facts.c` against a fixture chat.db with 50 messages spanning all variations
- **Size:** ~200 LOC src + ~150 LOC test
- **Depends on:** T-F1

## Phase G — Verification

### T-G1: Full suite green
- **AC:** AC-9
- **Verify:** `cmake --preset dev && cmake --build --preset dev && ./build/human_tests` → 0 failures, 0 ASan errors
- **Depends on:** all prior

### T-G2: Manual end-to-end on macOS
- **AC:** AC-4 / AC-5 / AC-6 acceptance via real send
- **Steps:**
  1. Enable `action_surface_v2` in config.json
  2. Restart daemon
  3. Send Seth a question → expect threaded reply with `reply_to_guid` populated in chat.db
  4. Send Seth multiple rapid messages → expect mostly flat sends (density damping)
  5. Send Seth an emotional message → expect TAPBACK_PLUS_FLAT or FLAT, never TAPBACK solo
  6. Drop 5 stickers into `~/.human/stickers/` → expect a sticker send within ~50 conversational turns
  7. Tail `~/.human/logs/imessage_action.jsonl` → confirm every reply has a logged decision
- **Depends on:** T-G1

### T-G3: Telemetry review + log-odds tuning (post-deploy)
- **AC:** AC-2 (real-world distribution sanity)
- **Steps:** after 1 week of telemetry, sample 100 random lines from `imessage_action.jsonl`, hand-grade style fit, tune `thread_logodds` weights if distribution looks off, log changes in a follow-up ADR
- **Depends on:** T-G2 + 7 days of usage

## Out of scope this sprint (tracked for follow-up)

- ❌ Forward action (separate spec: `2026-XX-imessage-forward`)
- ❌ Delete action (separate spec: `2026-XX-imessage-delete`)
- ❌ True balloon-bundle stickers (blocked until macOS 26+ entitlement story changes)
- ❌ Propagation of `reply()` to Telegram / Discord / Slack (separate spec: `2026-XX-imessage-action-surface-multichannel`)

## Risk-tier summary

| Phase | Risk tier | Why |
|-------|-----------|-----|
| A | Low | Pure C math, no I/O |
| B | Medium | Vtable changes ripple through 42 channels (script-driven, mechanical) |
| C | Medium | AX automation — but proven shape (mirrors tapback) |
| D | Medium | Sub-picker AX is newer in Sonoma+ — locale-label risk |
| E | Low | Just a file scan + attachment send via existing imsg path |
| F | Medium | New dispatcher in daemon — touches the reply hot path |
| G | Low | Verification |
