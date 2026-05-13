---
title: "iMessage Shape Refactor — Carving Modules from imessage.c"
created: 2026-05-12
status: active
scope: src/channels/imessage*.c
related: docs/investigations/imessage-capability-matrix.md
---

# iMessage Shape Refactor

## Problem

`src/channels/imessage.c` is 4,426 LOC and growing. Every Tier‑1 capability is
in there — text send, media send, AppleScript/imsg dual‑path, AX tapback, AX
typing, chat.db poll, schema detection, attributedBody parsing, sticker /
balloon / effect classification, GIF fetch + JSON parse, music tapback,
read‑receipt context, contact graph touches, circuit breaker, watchdog. The
pattern that produced this is the slow one: features were appended rather than
carved.

Symptoms felt by anyone editing this file:

- Locating the right call site requires a long search every time.
- "Touch nothing else" diffs balloon because clang‑format reformats unrelated
  regions every commit.
- Reviewers can't tell whether a change is local or system‑wide.
- Function ownership is unclear (e.g. `imsg_watch_*` is a subsystem inside a
  channel file; doctor / observability code straddles two layers).

The capability surface is **mature**; the *shape* is immature. This plan
carves the file into single‑purpose modules without changing behavior, so the
next feature wave doesn't ossify it further.

## Non‑goals

- **No behavior change.** Every test must pass before and after each step.
- **No public API change.** `include/human/channels/imessage.h` is the
  contract; consumers don't move.
- **No feature additions.** The audit's W3 (voice memo transcription), W5
  (auto‑recovery escalation), and shape items that overlap with feature work
  belong in their own PRs.
- **No `HU_IMESSAGE_*_ENABLED` flag changes** during the refactor. Sequencing
  matters; flag changes mix concerns.

## Target shape

```
src/channels/
├── imessage.c              ← vtable, factory, ctx, lifecycle (target: ~600 LOC)
├── imessage_internal.h     ← cross‑module ctx + helper signatures (NEW)
├── imessage_classify.c     ← pure lookup/data helpers (DONE, ~100 LOC)
├── imessage_watch.c        ← imsg subprocess lifecycle (~140 LOC)
├── imessage_poll.c         ← chat.db poll, schema cache, message parsers (~900 LOC)
├── imessage_send.c         ← text / media send (imsg + AppleScript paths) (~600 LOC)
├── imessage_react.c        ← tapback 3‑tier fallback + JXA / AX glue (~500 LOC)
├── imessage_typing.c       ← AX typing + simulated typing (~250 LOC)
├── imessage_attachment.c   ← GIF / image / read‑receipt context (~500 LOC)
└── imessage_ax.c           ← AX framework wrappers (find window/row/etc.) (~400 LOC)
```

Result: ten ~average‑sized C files instead of one 4400‑LOC monolith. Each is
testable in isolation; reviewers can grok a diff at a glance.

## Sequence

Each step is **independently mergeable**, with explicit invariants. Run the
full `human_tests` suite + `scripts/check-untested.sh` between every step.

### Step 0 — `imessage_classify.c` (✅ DONE in this commit)

Pure lookup helpers (~100 LOC, 5 functions). Zero state, zero platform code.
Proves the mechanics (new file + CMakeLists wiring + public‑header contract)
with the lowest possible regression risk.

**Risk:** trivial. **Behavior change:** none. **Public API change:** none.

### Step 1 — `imessage_internal.h` + `imessage_ax.c`

Carve the Accessibility (AX) framework wrappers into one place. Today the
`ax_*` functions are sprinkled through imessage.c and each new tapback / typing
feature adds more. Centralizing them is a pre‑req for separating react and
typing.

Functions to move:

- `ax_get_messages_window` (line 2748, 55 LOC)
- `ax_find_compose_field_recurse` (line 2613, 61 LOC)
- `ax_find_message_group` (line 2868, 73 LOC)
- `ax_open_conversation` (line 2694, 49 LOC)
- `ax_messages_pid` (line 2588, 21 LOC)

Plus the AX‑specific helpers used only by tapback/typing. Total ~400 LOC.

Create `src/channels/imessage_internal.h` to hold cross‑module signatures
(not part of the public API). Mark every exported function `HU_INTERNAL` (a
new macro that expands to `__attribute__((visibility("hidden")))` so we don't
accidentally widen the public surface).

**Risk:** medium — AX is `__APPLE__`‑only and has complex header dependencies.
**Behavior change:** none. **Public API change:** none. Internal header is
new.

### Step 2 — `imessage_typing.c`

Extract typing. Functions to move:

- `imessage_simulate_typing` (line 1108, 147 LOC)
- `imessage_start_typing` (line 3158, 62 LOC) — vtable hook stays pointed at it
- `imessage_stop_typing` (the matching stop)
- `ax_start_typing` (line 2805, 39 LOC) — moves with the rest

Total ~250 LOC. Depends on `imessage_ax.c` (Step 1).

**Risk:** low. **Behavior change:** none.

### Step 3 — `imessage_watch.c`

Extract the imsg subprocess. Functions to move:

- `imsg_watch_start` (line 657, 53 LOC)
- `imsg_watch_stop` (line 749, 20 LOC)
- `imsg_watch_has_data` (line 715, 33 LOC)
- `imsg_validate_target` (line 772, 39 LOC)
- `imsg_cli_available` (line 638, 17 LOC)

Total ~165 LOC. Self‑contained subprocess lifecycle. State lives in
`hu_imessage_ctx_t`; pass it through.

**Audit follow‑up bundled in:** add `SIGCHLD` reaper + audit every early
return for pipe / fd cleanup (audit item B5).

**Risk:** medium — subprocess lifecycle is the kind of thing that crashes in
prod, not in tests. **Behavior change:** the SIGCHLD / cleanup fix IS a
behavior change. Justified.

### Step 4 — `imessage_react.c`

Extract tapback. Functions to move:

- `imessage_react` (line 2194, 266 LOC) — the 3‑tier fallback orchestrator
- `imessage_reaction_to_ax_action_prefix` (line 1027, 22 LOC)
- `ax_perform_tapback_on_row` (line 2947, 38 LOC)
- Build helpers for tapback context (used by poll for *read* of tapbacks)

Total ~350 LOC. Depends on `imessage_ax.c`.

**Audit follow‑up bundled in:** the 3‑tier fallback (imsg → JXA+AX → fail)
becomes an explicit `tapback_strategy_t` enum with a small dispatch table,
not nested conditionals (audit item C4).

**Risk:** medium — tapback is fragile to macOS version drift. **Behavior
change:** strategy enum is a refactor, not new behavior.

### Step 5 — `imessage_poll.c`

The big one. Carve `hu_imessage_poll` (415 LOC) + helpers + the
`IMSG_POLL_SQL_BASE` macro. Functions to move:

- `hu_imessage_poll` (line 3573, 415 LOC)
- `imessage_open_chatdb` (line 547, 21 LOC)
- The chat.db SQL macro `IMSG_POLL_SQL_BASE` (line 3717, ~75 LOC) — move into
  its own `imessage_poll_sql.h` header.
- `hu_imessage_get_attachment_path` (line 3412, 65 LOC)
- `hu_imessage_get_latest_attachment_path` (line 3486, 74 LOC)
- `hu_imessage_lookup_message_by_guid` (line 4176, 67 LOC)
- `hu_imessage_user_responded_recently` (line 569, 62 LOC)
- attributedBody parse + sticker / balloon / effect classification (already
  pulled out partially in `imessage_classify.c`)

Total ~900 LOC.

**Audit follow‑ups bundled in:**

- C6: schema column detection cache (`SELECT date_retracted FROM message
  LIMIT 0`) moves to a one‑shot at ctx init, not every poll.
- C5: `IMSG_POLL_SQL_BASE` becomes its own header for schema‑diff testing.
- B2: defensive null check on `sqlite3_column_blob` return.
- C3: poll() decomposes into `imessage_poll_query`, `imessage_parse_message`,
  `imessage_classify_balloon`, `imessage_classify_effect`.

**Risk:** high — poll is the chat.db read hot path. **Behavior change:** the
schema‑cache + null‑guard are real fixes; document in the commit.

### Step 6 — `imessage_send.c`

Carve text/media send. Functions to move:

- `imessage_send` (line 1259, 303 LOC) — the dual‑path orchestrator
- AppleScript send helpers
- imsg CLI send helpers
- `imessage_sanitize_output` (line 905, 28 LOC) — depends on
  `hu_conversation_strip_ai_phrases`, moves cleanly with send

Total ~600 LOC.

**Audit follow‑up bundled in:** C2 — `if (use_imsg_cli) { ... } else { ... }`
collapses to a sub‑vtable (`imessage_send_strategy_t` with `send_text` /
`send_media` slots; imsg CLI and AppleScript are two implementations).

**Risk:** medium. **Behavior change:** strategy struct is refactor.

### Step 7 — `imessage_attachment.c`

Carve GIF / image / music / read‑receipt context. Functions to move:

- `hu_imessage_fetch_gif` (line 4033, 128 LOC) — also fixes audit B6 (JSON
  parse via `human/core/json.h`)
- `hu_imessage_count_recent_gif_tapbacks` (line 1879, 55 LOC)
- `hu_imessage_count_recent_music_tapbacks` (line 1935, 53 LOC)
- `hu_imessage_build_read_receipt_context` (line 2032, 108 LOC)
- `hu_imessage_build_tapback_context` (line 1768, 110 LOC)

Total ~500 LOC.

**Risk:** low. **Behavior change:** JSON‑parse fix (B6) is real; bundle with
attachment carve.

### Step 8 — clean up `imessage.c`

What remains in `imessage.c`:

- `hu_imessage_create` factory (line 3292, 70 LOC)
- `hu_imessage_ctx_t` struct definition (or move to `imessage_internal.h`)
- vtable struct + tiny vtable‑hook trampolines that delegate to module
  functions
- `imessage_health_check`
- `hu_imessage_watchdog_tick` (line 411, 47 LOC) — stays here as it owns
  channel‑level state
- Circuit breaker bookkeeping (`imessage_record_open_result` etc.)

Target after Step 8: ~600 LOC. **Risk:** low. **Behavior change:** none.

## Risk register

| Risk | Mitigation |
|---|---|
| Behavior regression from accidental scope creep | Each step is a separate commit; full test suite between each |
| Linker errors from static→external transitions | Use `HU_INTERNAL` macro + `imessage_internal.h`; never widen public surface |
| macOS version drift in AX paths | Keep AX functions in one file (Step 1); easier to version‑gate |
| Pre‑commit clang‑format reformatting unrelated regions | Each commit explicitly notes formatter‑touched lines |
| Breaking iMessage e2e (`scripts/e2e-imessage-humanness.sh`) | Run e2e‑live on macOS host between each step on a fixture target |
| chat.db schema drift on macOS 15+ during poll carve | Step 5 is the highest‑risk; do it last, after Steps 0–4 land |

## Definition of done

- [ ] All 8 steps merged.
- [ ] `imessage.c` is **≤ 700 LOC**.
- [ ] No file in `src/channels/imessage*` is **> 1000 LOC**.
- [ ] Public API (`include/human/channels/imessage.h`) is byte‑identical to
      pre‑refactor.
- [ ] `human_tests` suite: 100% pass with zero ASan errors at every step.
- [ ] e2e‑imessage‑humanness.sh: green on macOS host at every step.
- [ ] `docs/investigations/imessage-capability-matrix.md` updated with the
      new file map.
- [ ] Audit items B2, B5, B6, C2, C3, C4, C5, C6 closed (bundled into the
      relevant steps as noted).

## Sequence rationale

Sorted by **risk × dependency**:

1. **Step 0 (classify)** — proof of mechanics. ✅ DONE.
2. **Step 1 (AX)** — pre‑req for 2 and 4. Centralizes the
   platform‑specific surface.
3. **Step 2 (typing)** — smallest module that depends on AX. Safe second.
4. **Step 3 (watch)** — fully independent of AX. Could be parallel with 2.
5. **Step 4 (react)** — depends on AX. Bigger but well‑contained.
6. **Step 6 (send)** — independent of AX. Could be parallel with 4 or 5.
7. **Step 7 (attachment)** — independent. Last of the "easy" carves.
8. **Step 5 (poll)** — highest‑risk; runs last when the rest of the file is
   slim enough that scope creep is impossible.
9. **Step 8 (cleanup)** — bookkeeping after all carves land.

## Owner / cadence

- One step per PR. Each PR is reviewable in < 30 minutes.
- Sequence is strict for Steps 0 → 1 → 2 → 3. Steps 4 / 5 / 6 / 7 can ship
  in any order once 1 lands.
- Step 5 must land last regardless of order of 4 / 6 / 7.
- Expected calendar: 1 PR / day = 8 days from Step 0 to done.

## Out of scope (handled separately)

- **W3 — voice memo transcription.** Touches send/attachment paths but is a
  feature, not shape. Land after Step 7.
- **W5 — doctor → recovery auto‑fix.** Touches `doctor.c` and `doctor_fix.c`,
  not iMessage. Landed in the same PR series as Insight 3.
- **iMessage naturalness eval baseline.** No code change; a process step.
- **4426‑LOC god‑file disclaimer.** Until the refactor lands, `imessage.c`
  carries a header pointing here.
