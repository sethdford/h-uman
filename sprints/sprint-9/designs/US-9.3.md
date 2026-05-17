# Design for US-9.3: iMessage non-allowlisted sender reply (P1, Sprint 9)

## Story summary

When an iMessage DM arrives from a handle not in `c->allow_from`, the
current code at `src/channels/imessage.c:3905-3908` silently advances
`last_rowid` and drops the message. The sender sees nothing; the operator
sees nothing. This story replaces the silent drop with:

1. **A one-time courtesy reply** to the sender (per handle per 24h).
2. **An operator-side log line** every time a non-allowlisted DM is
   observed (every occurrence, not gated by the 24h dedup — operators
   need to see ongoing volume).
3. **A SQLite-locked rate-limit table** so the reply cap survives daemon
   restarts and resists spoof-spam.

We also align AC-9.3.3 (chat.db SQLITE_BUSY warning + immediate
`hu_imessage_save_poll_status`) with the same touch on the poll loop —
it lives in the same function and is a one-liner per the AC, so we
bundle it rather than splitting.

## Approach

### Why a persistent dedup table, not in-memory

In-memory dedup (a small ring on `hu_imessage_ctx_t`) is the cheapest
option and would satisfy the literal AC text. We reject it because:

- The daemon restarts often during development and on macOS reboots; a
  ring would let a single attacker spray once per restart cycle.
- The "weaponized as a spam vector" risk in the brief is the dominant
  consideration. Persistence across restarts is part of the mitigation.
- We already have a `~/.human/` directory pattern (status file at line
  ~163) for small per-channel state files. Adding one more newline-
  delimited file (`imessage_courtesy.log`) is trivial and inspectable
  with `cat`, matching the precedent set by the poll-status file.

The dedup key is `handle + floor(epoch / 86400)` (AC-9.3.1 dictates this
exact bucketing). The file contains one line per `(bucket,handle)` pair;
on each non-allowlisted message we append-if-absent. Truncation policy:
keep the last 256 lines (a single ftruncate at write time). 256 entries
× ~64 bytes = ~16KB worst case — small enough that a linear scan on
every non-allowlisted message is fine. We do not optimize this until
we have a measured caller.

### Why expose the decision as a pure predicate

Per `.claude/rules/security-predicate-extraction.md`: the decision
"should we courtesy-reply this handle right now?" depends on (a) is the
handle in the allowlist, (b) has the same handle already received a
courtesy reply in the current 24h bucket, (c) is the courtesy reply
feature enabled by config. That's three boolean facts. We extract:

```
bool hu_imessage_should_courtesy_reply(
    bool allowlist_has_handle,        /* false → handle is non-allowlisted */
    bool dedup_already_replied,        /* true  → already replied this bucket */
    bool courtesy_replies_enabled,     /* config flag, default true */
    bool persona_disclosure_allowed);  /* false → strip persona name */
```

This predicate is testable without forking the daemon, without writing
to chat.db, without a real persona. The production poller calls the
same predicate. The truth table is 4 inputs (16 rows, but most collapse
— see test plan). The reply *text builder* is a separate pure function
(`hu_imessage_build_courtesy_reply`) that takes the persona name (or
NULL → generic fallback) and the owner handle (or NULL → omit).

### Why we do NOT call `imessage_send` directly from the poller

The poll loop currently produces a batch of accepted messages and
returns to the agent loop; the *agent* then composes a reply via
`imessage_send`. Calling `imessage_send` from inside the poller would
re-enter the channel vtable from itself and skip the echo-prevention
ring (`imessage_was_sent_by_us`, line 3883). Instead we add a
side-channel field on `hu_imessage_ctx_t` — `pending_courtesy[N]`
buffer of `(handle, reply_text)` pairs — that the agent loop drains at
the same point it would otherwise drain `c->mock_msgs`. The send goes
through the normal `imessage_send` path, the echo-ring is populated,
and a real reply from the (now-allowlisted) sender won't be mistaken
for an echo.

Under `HU_IS_TEST`, the same buffer is what the test asserts against
(it already exists as `last_message` for outbound; we either extend
that field or add `last_courtesy_message[4096]` parallel to it — the
implementer picks the lower-LOC option after looking at how
`imessage_send` populates `last_message`).

### AC-9.3.3 (chat.db BUSY warning) — same poll loop, one-liner

The retry loop at lines 553-569 already exhausts after 3 attempts. We
add at the exhaustion site:

1. Call `hu_imessage_save_poll_status` (already exists).
2. Emit `hu_log_warn("chat.db busy after 3 retries — Messages.app may be syncing")`
   (gated by a one-shot bool similar to `breaker_log_emitted` so we
   don't spam every poll tick).

## Files to modify

| File | Change | Est LOC |
|---|---|---|
| `include/human/imessage.h` (or wherever `hu_imessage_ctx_t` decls live; otherwise the struct is private in `imessage.c`) | Add prototype for `hu_imessage_should_courtesy_reply` and `hu_imessage_build_courtesy_reply`. | +20 |
| `src/channels/imessage.c` | Add pure predicate, reply text builder, dedup file read/write/truncate, pending-courtesy buffer drain hook, fold predicate into the line 3905 non-allowlisted branch, add AC-9.3.3 BUSY-exhaustion warn + save_poll_status, add one-shot log gate field. | +180 |
| `src/channels/imessage.c` (struct) | Add `bool courtesy_replies_enabled` (default true), `bool chatdb_busy_log_emitted`, `pending_courtesy[N]` (N=4) array with handle+text, and a test-only `last_courtesy_message[4096]` field. | +25 (within the struct) |
| `tests/test_imessage_non_allowlisted.c` | NEW test file. References `hu_imessage_*` production symbols (AC-9.3.5). 10 test cases — see test plan below. | +320 |
| `tests/CMakeLists.txt` (or wherever test sources are registered) | Register the new test file. | +1 |
| `docs/error-codes.md` | No new error code; courtesy-reply path is best-effort, returns void from the predicate. | 0 |

Total: ~+545 LOC, ~+545 changed.

## Implementation steps (for the implementer agent)

1. **Skeleton (no behavior).** Add the two prototypes and empty bodies
   that return `false` / produce an empty string. Add the new fields to
   `hu_imessage_ctx_t`. Verify the project still compiles with
   `cmake --build --preset dev`.
2. **Truth-table tests first (TDD).** Write the 7 predicate tests in
   `tests/test_imessage_non_allowlisted.c`. They will all fail. This
   pins the contract before any logic is written.
3. **Implement `hu_imessage_should_courtesy_reply`** so the 7 predicate
   tests pass. No I/O — pure boolean logic.
4. **Implement `hu_imessage_build_courtesy_reply`** so the 2 text-builder
   tests pass. Pure string composition into a caller-provided buffer.
   Must NEVER include the operator's real name — only "the operator" or
   the persona name. Must be safe on persona_name=NULL.
5. **Dedup-file helpers.** Write `imessage_courtesy_dedup_check(handle, bucket)` and
   `imessage_courtesy_dedup_record(handle, bucket)` as static
   functions in `imessage.c`. Path: `$HOME/.human/imessage_courtesy.log`.
   Write the 3 dedup-file tests (uses tmpdir HOME via existing test
   pattern in `test_imessage_chatdb_fixture.c`).
6. **Wire predicate + dedup into the line 3905 branch.** Replace the
   silent `continue` with:
   - Always: emit `hu_log_info("non-allowlisted iMessage from %s; dropping", handle)` (operator-side, every occurrence).
   - Compute bucket = epoch / 86400. Compute `dedup_already_replied = imessage_courtesy_dedup_check(handle, bucket)`.
   - Call predicate; if true → build reply text, push to `pending_courtesy` ring, call `imessage_courtesy_dedup_record`.
   - Still advance `last_rowid` and continue (do NOT pass the message to the agent).
7. **Drain `pending_courtesy` in the agent loop.** In whatever place
   the agent loop next iterates after a poll, send each pending
   courtesy via `imessage_send`. Under `HU_IS_TEST`, the existing
   `imessage_send` mock records to `last_message` — the test mirrors to
   `last_courtesy_message` so allowlisted-reply tests still work.
8. **AC-9.3.3.** At the SQLITE_BUSY retry-exhaustion site (~line 569),
   call `hu_imessage_save_poll_status(c)` and emit the warn line gated
   by `c->chatdb_busy_log_emitted`. Reset the one-shot when a poll
   succeeds.
9. **Mock-epoch seam.** Add `int64_t mock_epoch_override` (only inside
   `HU_IS_TEST`) to `hu_imessage_ctx_t`, defaulting to 0 → use real
   `time(NULL)`. Tests set it to control buckets.
10. **Integration test.** Write the 3 end-to-end tests (inject mock
    message → assert `last_courtesy_message` set → inject second mock
    → assert no second write → advance mock_epoch by 86400 → inject
    third → assert second courtesy reply).
11. **Run full suite** (`./build/human_tests`). Must be 0 failures,
    0 ASan. Spawn `/verify` agent. Capture `RESULT_verifier=PASS`.

## Risks

- **Spam vector (MEDIUM / MEDIUM-LARGE)** — Attacker spoofs many handles
  and sprays the daemon. Each spoofed handle gets one courtesy reply
  per 24h. With 10,000 spoofed handles per day, that's 10K outbound
  iMessages — visible volume and an Apple TOS concern.
  *Mitigations applied in design*: (a) per-handle 24h cap, (b)
  persistent dedup across daemon restarts, (c) config flag
  `courtesy_replies_enabled` to disable entirely, (d) the courtesy
  reply *does not* reveal the operator's real handle — only "the
  operator" or the configured `owner_display_name`, (e) we will add
  an aggregate cap (max 50 courtesy replies per 24h across all handles)
  in the predicate via a counter in the same dedup file. Even with
  spoofing, we send at most 50/day.
- **Sender-identity leak in the reply text (LOW / MEDIUM)** — the AC
  text says "ask [owner handle] to add you". A naive implementer might
  put the operator's phone number / Apple ID in the reply. *Mitigation*:
  the builder API takes `owner_display_name`, not `owner_handle`. If
  unset, we say "the operator" only. A test asserts the reply never
  contains characters typical of phone numbers or @-domains.
- **Concurrency (LOW / SMALL)** — Two daemons running against the same
  `~/.human/` directory would race on the dedup file. We use
  `O_APPEND` for the record path and a `flock(LOCK_EX, LOCK_NB)` for
  the truncation step; on lock failure we skip the truncate. Reads do
  not need the lock — worst case is a duplicate reply, which is a
  far smaller failure than corruption.
- **Tests-pin-bugs trap (HIGH / SMALL — explicit per the brief)** — Per
  `.claude/rules/tests-that-pin-bugs.md`: the test must assert the
  *reply was sent*, NOT that the function returned 0 / that the rowid
  advanced. Test assertions:
  - `HU_ASSERT_TRUE(c->last_courtesy_message[0] != '\0');`
  - `HU_ASSERT_STR_CONTAINS(c->last_courtesy_message, "allowlist");`
  - `HU_ASSERT_TRUE(strstr(c->last_courtesy_message, "ask") != NULL);`
  *Anti-pattern to avoid*: `HU_ASSERT_EQ(rc, HU_OK)` is NOT enough — the
  current buggy code path also returns OK while silently dropping.
- **Backward compatibility (LOW / SMALL)** — `courtesy_replies_enabled`
  defaults to true. Existing operators with a non-empty allowlist will
  immediately start sending courtesy replies. *Mitigation*: documented
  release note + config opt-out. Operators with `allow_from = ["*"]`
  are unaffected (the wildcard path matches before the courtesy
  branch).
- **Observability (LOW / SMALL)** — The `hu_log_info` line per drop
  satisfies the "operator-side log" requirement. We do NOT add a metric
  counter; the dedup file IS the count (lines correspond to replies
  sent). A future story can expose `human doctor` view if needed.
- **AC-9.3.3 cross-coupling (LOW / SMALL)** — Bundling BUSY-warning
  changes with allowlist changes mixes two concerns. Justification:
  same file, same function, same poll loop, ~6 LOC, both AC-blocking
  for this story. Splitting into two commits inside one PR is
  acceptable; splitting into two PRs is overhead with no win.

## Test strategy

All tests in `tests/test_imessage_non_allowlisted.c`. The file uses
the existing `hu_imessage_*` symbols (satisfies AC-9.3.5 and
`.claude/rules/test-references-production-symbol.md`). No
`// @covers-none` escape.

### Predicate truth table (7 tests)
1. `non_allowlisted_first_time_returns_true` — `allowlist_has_handle=false, dedup_already_replied=false, enabled=true` → predicate returns true.
2. `non_allowlisted_second_time_in_bucket_returns_false` — same as above but `dedup_already_replied=true` → false.
3. `allowlisted_handle_returns_false` — `allowlist_has_handle=true` → false regardless of other inputs.
4. `disabled_by_config_returns_false` — `enabled=false` → false.
5. `predicate_is_pure_no_side_effects` — call 100 times with same inputs, assert no state mutation (no I/O, no log line).
6. `predicate_handles_persona_disclosure_off` — `persona_disclosure_allowed=false` → still returns true/false based on the gate inputs (this knob feeds the *builder*, not the gate; predicate ignores it).
7. `predicate_all_false_returns_false` — paranoia case.

### Text-builder tests (3 tests)
8. `reply_text_mentions_allowlist_word` — output contains "allowlist".
9. `reply_text_omits_owner_handle_when_only_display_name_given` — pass `owner_display_name="Jane"`, `owner_handle=NULL` → output must contain "Jane", must NOT contain `+1` or `@`.
10. `reply_text_safe_with_null_persona_name` — pass `persona_name=NULL` → no crash, output uses generic phrasing.

### Dedup file tests (3 tests)
11. `dedup_record_then_check_returns_true_same_bucket` — record handle in bucket B, check same handle in bucket B → true.
12. `dedup_check_returns_false_for_next_bucket` — record in bucket B, check in bucket B+1 → false.
13. `dedup_file_truncates_at_256_entries` — write 300 entries, assert file is ≤256 lines after the next record.

### Integration tests (3 tests)
14. `non_allowlisted_first_dm_triggers_courtesy_reply` (AC-9.3.1, AC-9.3.4) — inject mock message from non-allowlisted handle, drive one poll, assert `last_courtesy_message` populated, assert content contains "allowlist".
15. `non_allowlisted_second_dm_within_24h_no_reply` (AC-9.3.2, AC-9.3.4) — inject, drive poll, clear `last_courtesy_message`, inject again with same mock_epoch, drive poll, assert `last_courtesy_message[0] == '\0'`.
16. `non_allowlisted_dm_in_next_bucket_triggers_second_reply` — inject, drive poll, advance `mock_epoch_override` by 86400, inject again, drive poll, assert `last_courtesy_message` populated again.

### AC-9.3.3 tests (2 tests)
17. `chatdb_busy_exhaustion_emits_warn_and_saves_poll_status` — mock `sqlite3_step` to return SQLITE_BUSY 3 times; assert `hu_imessage_save_poll_status` was called (poll-status file exists with the expected fields) AND a warn line containing "busy" and "3 retries" was emitted (use existing log-capture seam if present, else assert via `last_logged_health` transition).
18. `chatdb_busy_log_is_one_shot_per_episode` — drive 10 consecutive BUSY-exhaustion ticks; assert exactly ONE warn line. Successful poll resets the gate.

Total: 18 tests. No real network. No process spawning. Deterministic.

## Acceptance criteria mapping

| AC | Behavior | Test(s) |
|---|---|---|
| AC-9.3.1 | One reply sent on first non-allowlisted DM | 14 |
| AC-9.3.2 | No second reply within 24h | 15 |
| AC-9.3.3 | BUSY-after-3-retries → save_poll_status + warn line | 17, 18 |
| AC-9.3.4 | `last_message` (or `last_courtesy_message`) populated once per bucket | 14, 15, 16 |
| AC-9.3.5 | Test file references `hu_imessage_*` production symbols | enforced by `.githooks/pre-commit` via `scripts/check-test-references.sh` |

## Out of scope (carry-forwards if needed)

- "Add me" auto-allowlist from DM — explicitly forbidden by the iMessage
  MCP injection-resistance guidance. Stay out.
- Surfacing the dedup file via `human doctor` — future story; the file
  is already inspectable with `cat`.
- Localizing the courtesy reply text — English-only for now; persona
  language settings are a separate concern.

## Closing notes

- Risk tier: **MEDIUM** (matches the story header).
- One-concern-per-change: this PR is "the silent-drop is replaced with
  a rate-limited courtesy reply and a BUSY warning". Both changes live
  in the same poll function and are paired in the AC list.
- Anti-pattern guard: every test asserts a *positive* observable (reply
  sent, log emitted, file written) — none assert "function returned
  OK", per `tests/test_imessage_non_allowlisted` lessons.
