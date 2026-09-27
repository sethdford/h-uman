---
title: Voice Safety Parity Implementation Plan (W1)
description: Voice memos must pass moderation, companion safety and claim checks before any send.
category: plans
---

# Voice Safety Parity Implementation Plan (W1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A reactive reply can only go out as a voice memo if it passes the same moderation, companion-safety and claim-language gates the text path applies; otherwise voice is declined and the text path handles it.

**Architecture:** A pure predicate `hu_voice_reply_gates_clear()` in `src/daemon/daemon_voice_reply.c` runs the three existing gate functions. `hu_daemon_voice_reply()` calls it as its first statement and returns `false` (no voice sent) on any flag, so control falls through to the existing text path at `src/daemon.c:8691`, which already performs replacement, crisis escalation and hedging.

**Tech Stack:** C11, CMake, project test framework (`tests/test_framework.h`, `HU_RUN_TEST`).

**Spec:** `docs/superpowers/specs/2026-09-26-voice-memo-realism-design.md` (section W1)

## Global Constraints

- C11, `-Wall -Wextra -Wpedantic -Werror`; free every allocation (ASan clean).
- Tests: no network, no process spawning, deterministic.
- Never log reply text (it is user content); log only the gate name.
- One concern per change; conventional commit messages; commits end with the Co-Authored-By trailer.
- Work only in `/Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism` (absolute paths in every Bash call).

## Review Focus

- **Gate function returns an error** (not a flag) → voice must be declined (fail closed), not allowed. Pinned in Task 1 (NULL text / zero length).
- **Crisis content** ("I want to kill myself" style self-harm) → moderation flags `self_harm`; voice declined so the text path's crisis escalation runs. Pinned in Task 1.
- **Clean everyday reply** must still be allowed, or voice silently dies for everyone. Pinned in Task 1 (positive control).
- **Claim language** ("I remember when you…") → declined, because voice cannot carry the text path's hedge. Pinned in Task 1.
- **Gate placed after a send path** in a future edit → structural check in Task 2 (gate is the first statement of `hu_daemon_voice_reply`).

---

### Task 1: `hu_voice_reply_gates_clear` predicate

**Files:**
- Modify: `include/human/daemon.h` (declare next to `hu_daemon_voice_reply`, line ~96)
- Modify: `src/daemon/daemon_voice_reply.c` (define above `hu_daemon_voice_reply`)
- Test: `tests/test_daemon_voice_reply.c` (existing file, registered unconditionally at `CMakeLists.txt:4248`)

**Interfaces:**
- Consumes: `hu_moderation_check(hu_allocator_t*, const char*, size_t, hu_moderation_result_t*)` (`human/security/moderation.h`); `hu_companion_safety_check(hu_allocator_t*, const char*, size_t, const char*, size_t, hu_companion_safety_result_t*)` (`human/security/companion_safety.h`); `hu_memory_has_claim_language(const char*, size_t)` (`human/memory/verify_claim.h`).
- Produces: `bool hu_voice_reply_gates_clear(hu_allocator_t *alloc, const char *text, size_t text_len, const char **reason_out);` — `true` only when every gate ran and none flagged; `*reason_out` (if non-NULL) is set to a static string: `"clear"`, `"invalid"`, `"moderation"`, `"companion_safety"`, or `"claim_language"`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_daemon_voice_reply.c` above `run_daemon_voice_reply_tests`:

```c
static void test_voice_gates_allow_clean_reply(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "hey how's your day going? did you finish that project?";
    const char *why = NULL;
    HU_ASSERT_TRUE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), &why));
    HU_ASSERT_STR_EQ(why, "clear");
}

static void test_voice_gates_decline_moderation_flag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "kill them with violence and murder";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), &why));
    HU_ASSERT_STR_EQ(why, "moderation");
}

static void test_voice_gates_decline_companion_safety_flag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "don't go, please stay, after everything we did you want to leave?";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), &why));
    HU_ASSERT_STR_EQ(why, "companion_safety");
}

static void test_voice_gates_decline_claim_language(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "I remember when you told me about your trip";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), &why));
    HU_ASSERT_STR_EQ(why, "claim_language");
}

static void test_voice_gates_fail_closed_on_invalid_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, NULL, 0, &why));
    HU_ASSERT_STR_EQ(why, "invalid");
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, "", 0, NULL));
}
```

Register them inside `run_daemon_voice_reply_tests`:

```c
    HU_RUN_TEST(test_voice_gates_allow_clean_reply);
    HU_RUN_TEST(test_voice_gates_decline_moderation_flag);
    HU_RUN_TEST(test_voice_gates_decline_companion_safety_flag);
    HU_RUN_TEST(test_voice_gates_decline_claim_language);
    HU_RUN_TEST(test_voice_gates_fail_closed_on_invalid_input);
```

Before relying on the fixtures, confirm each one still trips (or doesn't trip) its gate in the current code — they are copied from `tests/test_moderation.c:17`, `tests/test_companion_safety.c:186`, `tests/test_verify_claim.c:9`, `tests/test_companion_safety.c:13`. If the macro `HU_ASSERT_STR_EQ` does not exist in `tests/test_framework.h`, use `HU_ASSERT_TRUE(strcmp(why, "clear") == 0)`.

- [ ] **Step 2: Build and run — expect a compile failure** (function undeclared)

Run: `cmake --build /Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism/build --target human_tests -j8 2>&1 | grep -E "error:" | head`
Expected: `error: call to undeclared function 'hu_voice_reply_gates_clear'`
(First build of a fresh worktree: `cmake --preset dev` from the worktree root.)

- [ ] **Step 3: Implement** — declaration in `include/human/daemon.h` beside `hu_daemon_voice_reply`:

```c
/* True only when the reply passes every outbound gate the text path applies
 * (moderation, companion safety, claim language). Fails closed: invalid input
 * or a gate error returns false. `reason_out` receives a static string. */
bool hu_voice_reply_gates_clear(hu_allocator_t *alloc, const char *text, size_t text_len,
                                const char **reason_out);
```

Definition in `src/daemon/daemon_voice_reply.c` (add includes `human/security/moderation.h`, `human/security/companion_safety.h`, `human/memory/verify_claim.h` if not already present):

```c
bool hu_voice_reply_gates_clear(hu_allocator_t *alloc, const char *text, size_t text_len,
                                const char **reason_out) {
    const char *why = "invalid";
    bool clear = false;
    if (text && text_len > 0) {
        hu_moderation_result_t mod;
        memset(&mod, 0, sizeof(mod));
        hu_companion_safety_result_t cs;
        memset(&cs, 0, sizeof(cs));
        if (hu_moderation_check(alloc, text, text_len, &mod) != HU_OK || mod.flagged)
            why = "moderation";
        else if (hu_companion_safety_check(alloc, text, text_len, NULL, 0, &cs) != HU_OK ||
                 cs.flagged)
            why = "companion_safety";
        else if (hu_memory_has_claim_language(text, text_len))
            why = "claim_language";
        else {
            why = "clear";
            clear = true;
        }
    }
    if (reason_out)
        *reason_out = why;
    return clear;
}
```

- [ ] **Step 4: Run the suite for this file — expect PASS**

Run: `/Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism/build/human_tests --filter=voice_gates 2>&1 | tail -3`
Expected: 5 tests, 0 failures.

- [ ] **Step 5: Prove the tests discriminate** — temporarily change the body to `return true;` (after setting `*reason_out = "clear"`), rebuild, run `--filter=voice_gates`: expect the 4 decline/invalid tests to FAIL. Restore the implementation, rebuild, confirm PASS.

- [ ] **Step 6: Commit**

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism add include/human/daemon.h src/daemon/daemon_voice_reply.c tests/test_daemon_voice_reply.c
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism commit -m "feat(voice): add outbound safety gate predicate for voice replies

Voice memos bypassed moderation, companion safety and claim hedging, which
only ran on the text path. This predicate runs all three and fails closed.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Gate `hu_daemon_voice_reply` before any send path

**Files:**
- Modify: `src/daemon/daemon_voice_reply.c` (`hu_daemon_voice_reply`, first statements after the `(void)` casts, currently ~line 78)
- Modify: `src/daemon.c:8685` comment only (document that voice is gated)

**Interfaces:**
- Consumes: `hu_voice_reply_gates_clear` from Task 1.
- Produces: `hu_daemon_voice_reply` returns `false` without calling any channel or TTS when the gate declines.

- [ ] **Step 1: Insert the gate** as the first executable statement of `hu_daemon_voice_reply`, before `bool sent_voice = false;`:

```c
    /* SHIELD parity: the text path runs moderation/crisis, companion safety and
     * claim hedging inside `if (!sent_voice …)` in daemon.c, so a voice memo would
     * skip all three. Decline voice unless every gate is clear; the caller then
     * delivers the reply through the text path, which applies them. */
    {
        const char *gate_why = NULL;
        if (!hu_voice_reply_gates_clear(alloc, response, response_len, &gate_why)) {
            hu_log_info("voice", NULL, "voice declined by safety gate: %s", gate_why);
            return false;
        }
    }
```

Check `hu_log_info`'s actual signature in `include/human/core/log.h` and match the existing call style in this file (e.g., `hu_log_warn("human", agent ? agent->observer : NULL, ...)` in daemon.c); never pass `response` to the log.

- [ ] **Step 2: Update the comment at `src/daemon.c:8685`**:

```c
                    /* ── Voice decision: TTS when channel has voice_enabled.
                     * hu_daemon_voice_reply declines voice unless the reply passes
                     * the same safety gates the text path below applies. ───── */
```

- [ ] **Step 3: Structural wiring check** (both must hold; paste output into the task report):

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism
grep -rn 'hu_voice_reply_gates_clear' $W/src --include='*.c' | grep -v 'hu_voice_reply_gates_clear(hu_allocator_t'   # expect the call inside hu_daemon_voice_reply
awk '/^bool hu_daemon_voice_reply\(/{f=1} f&&/hu_voice_reply_gates_clear|->send\(|hu_cartesia_tts_synthesize|hu_voice_session_start/{print NR": "$0; n++} n==2{exit}' $W/src/daemon/daemon_voice_reply.c   # first hit MUST be the gate
```

- [ ] **Step 4: Full build + full suite**

Run:
```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism
cmake --build $W/build --target human human_tests -j8 2>&1 | grep -E "error:|Linking C executable" | head
$W/build/human_tests 2>/dev/null | grep -E "Results:"
```
Expected: links clean; `Results: N/N passed`, 0 failures, no ASan errors.

- [ ] **Step 5: Commit**

```bash
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism add src/daemon/daemon_voice_reply.c src/daemon.c
git -C /Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism commit -m "fix(voice): decline voice replies that fail outbound safety gates

A flagged reply now falls through to the text path, which applies the
moderation replacement, crisis escalation and claim hedging. Latent until
now: the iMessage channel has voice_enabled unset (67 decisions, 0 sent).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
