# Phase 1 — Channel Behavior-Class: Remove Hardcoded Channel Dispatch from Agent Core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move channel-identity knowledge out of the agent turn loop. The function `at_behavior_channel_class()` in `agent_turn.c:382-405` hardcodes 14 channel names via `memcmp` to compute a behavior class. Relocate that table into the **Channels** bounded context behind a stable lookup, so adding a channel never touches agent core.

**Architecture:** Introduce `hu_channel_behavior_class_for_name()` owned by `src/channels/`. Agent core calls it with the channel name it already has (`hu_channel_message_t.channel`). No plumbing change to the turn loop — only the *location of knowledge* moves. As a bonus this replaces 14 prefix `memcmp` checks (which have a latent substring-collision bug, e.g. `"imessage"` matches `"imessagebot"`) with exact-name matching.

**Tech Stack:** C11, CTest-style `HU_TEST_*` macros, CMake source lists.

**Behavior contract to preserve (from current `agent_turn.c:382-405`):**
- `"voice"` → 1
- `"email"`, `"imap"`, `"gmail"` → 3
- `"telegram" "discord" "slack" "mattermost" "matrix" "irc" "line" "lark" "messenger" "whatsapp" "imessage" "sms"` → 2
- anything else / NULL / empty → 0

---

## File Structure

- Create: `include/human/channels/behavior_class.h` — the contract (enum + lookup decl)
- Create: `src/channels/behavior_class.c` — the name→class table (Channels context owns it)
- Create: `tests/test_channel_behavior_class.c` — pins the classification contract
- Modify: `src/CMakeLists.txt` — register source + test
- Modify: `tests/test_main.c` — declare + call the new test runner
- Modify: `src/agent/agent_turn.c:382-405` — delegate to the new lookup

---

### Task 1: Define the behavior-class contract + write the failing test

**Files:**
- Create: `include/human/channels/behavior_class.h`
- Create: `tests/test_channel_behavior_class.c`

- [ ] **Step 1: Write the contract header**

```c
/* include/human/channels/behavior_class.h */
#ifndef HU_CHANNELS_BEHAVIOR_CLASS_H
#define HU_CHANNELS_BEHAVIOR_CLASS_H

#include <stddef.h>

/* Behavior class drives tone/length/formality defaults in the agent turn
 * loop (maps to hu_behavior_input_t.channel_class in behavior/policy.h).
 * Owned by the Channels bounded context: adding a channel updates the table
 * in src/channels/behavior_class.c, never the agent core. */
typedef enum hu_channel_behavior_class {
    HU_CHANNEL_BEHAVIOR_DEFAULT = 0,
    HU_CHANNEL_BEHAVIOR_VOICE = 1,
    HU_CHANNEL_BEHAVIOR_CHAT = 2,  /* IM / group chat */
    HU_CHANNEL_BEHAVIOR_EMAIL = 3,
} hu_channel_behavior_class_t;

/* Returns the behavior class for a canonical channel name (the factory key,
 * e.g. "imessage", "slack"). Exact, case-insensitive match. NULL/empty/unknown
 * → HU_CHANNEL_BEHAVIOR_DEFAULT (0). Returns int (not the enum) so existing
 * agent-core call sites that store an int keep their type. */
int hu_channel_behavior_class_for_name(const char *name, size_t name_len);

#endif /* HU_CHANNELS_BEHAVIOR_CLASS_H */
```

- [ ] **Step 2: Write the failing test**

```c
/* tests/test_channel_behavior_class.c */
#include "human/channels/behavior_class.h"
#include "test_harness.h"
#include <string.h>

static int klass(const char *s) {
    return hu_channel_behavior_class_for_name(s, strlen(s));
}

static void behavior_class_maps_voice(void) {
    HU_ASSERT_EQ(klass("voice"), HU_CHANNEL_BEHAVIOR_VOICE);
}
static void behavior_class_maps_email_family(void) {
    HU_ASSERT_EQ(klass("email"), HU_CHANNEL_BEHAVIOR_EMAIL);
    HU_ASSERT_EQ(klass("imap"), HU_CHANNEL_BEHAVIOR_EMAIL);
    HU_ASSERT_EQ(klass("gmail"), HU_CHANNEL_BEHAVIOR_EMAIL);
}
static void behavior_class_maps_chat_family(void) {
    HU_ASSERT_EQ(klass("imessage"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("slack"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("telegram"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("sms"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("whatsapp"), HU_CHANNEL_BEHAVIOR_CHAT);
}
static void behavior_class_unknown_is_default(void) {
    HU_ASSERT_EQ(klass("carrier_pigeon"), HU_CHANNEL_BEHAVIOR_DEFAULT);
    HU_ASSERT_EQ(hu_channel_behavior_class_for_name(NULL, 0), HU_CHANNEL_BEHAVIOR_DEFAULT);
    HU_ASSERT_EQ(hu_channel_behavior_class_for_name("", 0), HU_CHANNEL_BEHAVIOR_DEFAULT);
}
/* Pins the latent prefix-collision bug the old memcmp had: a name that merely
 * STARTS WITH a known channel must NOT inherit its class. */
static void behavior_class_rejects_prefix_collision(void) {
    HU_ASSERT_EQ(klass("imessagebot"), HU_CHANNEL_BEHAVIOR_DEFAULT);
    HU_ASSERT_EQ(klass("voicemail"), HU_CHANNEL_BEHAVIOR_DEFAULT);
}

void run_channel_behavior_class_tests(void) {
    HU_TEST_SUITE("channel_behavior_class");
    HU_RUN_TEST(behavior_class_maps_voice);
    HU_RUN_TEST(behavior_class_maps_email_family);
    HU_RUN_TEST(behavior_class_maps_chat_family);
    HU_RUN_TEST(behavior_class_unknown_is_default);
    HU_RUN_TEST(behavior_class_rejects_prefix_collision);
}
```

- [ ] **Step 3: Verify it fails to link (impl missing)**

Run: `touch tests/test_channel_behavior_class.c && cmake --build build --target human_tests -j8`
Expected: link error `undefined reference to hu_channel_behavior_class_for_name` (and `run_channel_behavior_class_tests` once wired in Task 3). This confirms the test exercises the real symbol.

---

### Task 2: Implement the lookup (make the test pass)

**Files:**
- Create: `src/channels/behavior_class.c`

- [ ] **Step 1: Write the table + exact-match lookup**

```c
/* src/channels/behavior_class.c */
#include "human/channels/behavior_class.h"
#include <strings.h> /* strncasecmp */

struct class_entry {
    const char *name;
    int klass;
};

/* Canonical channel names → behavior class. Mirrors the legacy table that
 * lived in agent_turn.c; this is now the single source of truth. Add a row
 * here when you add a channel — agent core never changes. */
static const struct class_entry k_table[] = {
    {"voice", HU_CHANNEL_BEHAVIOR_VOICE},
    {"email", HU_CHANNEL_BEHAVIOR_EMAIL},
    {"imap", HU_CHANNEL_BEHAVIOR_EMAIL},
    {"gmail", HU_CHANNEL_BEHAVIOR_EMAIL},
    {"telegram", HU_CHANNEL_BEHAVIOR_CHAT},
    {"discord", HU_CHANNEL_BEHAVIOR_CHAT},
    {"slack", HU_CHANNEL_BEHAVIOR_CHAT},
    {"mattermost", HU_CHANNEL_BEHAVIOR_CHAT},
    {"matrix", HU_CHANNEL_BEHAVIOR_CHAT},
    {"irc", HU_CHANNEL_BEHAVIOR_CHAT},
    {"line", HU_CHANNEL_BEHAVIOR_CHAT},
    {"lark", HU_CHANNEL_BEHAVIOR_CHAT},
    {"messenger", HU_CHANNEL_BEHAVIOR_CHAT},
    {"whatsapp", HU_CHANNEL_BEHAVIOR_CHAT},
    {"imessage", HU_CHANNEL_BEHAVIOR_CHAT},
    {"sms", HU_CHANNEL_BEHAVIOR_CHAT},
};

int hu_channel_behavior_class_for_name(const char *name, size_t name_len) {
    if (!name || name_len == 0) {
        return HU_CHANNEL_BEHAVIOR_DEFAULT;
    }
    for (size_t i = 0; i < sizeof(k_table) / sizeof(k_table[0]); i++) {
        const char *cand = k_table[i].name;
        size_t cand_len = 0;
        while (cand[cand_len] != '\0') {
            cand_len++;
        }
        if (cand_len == name_len && strncasecmp(name, cand, name_len) == 0) {
            return k_table[i].klass;
        }
    }
    return HU_CHANNEL_BEHAVIOR_DEFAULT;
}
```

- [ ] **Step 2: Run the suite to verify pass** (after Task 3 wiring)

Run: `touch src/channels/behavior_class.c && cmake --build build --target human_tests -j8 && ./build/human_tests --filter=behavior_class`
Expected: PASS, 5/5.

---

### Task 3: Register source + test runner in the build

**Files:**
- Modify: `src/CMakeLists.txt`
- Modify: `tests/test_main.c`

> Channels are NOT feature-gated, so register unconditionally (no `if(HU_ENABLE_*)`
> block) — satisfies `.claude/rules/test-source-gate-symmetry.md`.

- [ ] **Step 1: Add the source to the core library list**

In `src/CMakeLists.txt`, find the line registering another channels source (e.g. `src/channels/format.c`) and add alongside it:

```cmake
    src/channels/behavior_class.c
```

- [ ] **Step 2: Add the test source to the test list**

In `src/CMakeLists.txt`, find where `tests/test_*.c` are appended to the test sources and add:

```cmake
    tests/test_channel_behavior_class.c
```

- [ ] **Step 3: Declare + call the runner in `tests/test_main.c`**

Add the forward declaration alongside the other `run_*_tests` declarations:

```c
void run_channel_behavior_class_tests(void);
```

And add the call inside `main()` alongside the other channel test calls:

```c
    run_channel_behavior_class_tests();
```

- [ ] **Step 4: Build + run the targeted test**

Run: `cmake --build build --target human_tests -j8 && ./build/human_tests --filter=behavior_class`
Expected: PASS, 5/5.

- [ ] **Step 5: Commit**

```bash
git add include/human/channels/behavior_class.h src/channels/behavior_class.c \
        tests/test_channel_behavior_class.c src/CMakeLists.txt tests/test_main.c
git commit -m "feat(channels): own behavior-class lookup in channels context

Relocates channel-identity knowledge out of agent core. Exact-match
lookup replaces prefix memcmp (fixes latent prefix-collision)."
```

---

### Task 4: Rewire agent core to delegate (kill the `memcmp`)

**Files:**
- Modify: `src/agent/agent_turn.c:382-405`

- [ ] **Step 1: Replace the function body with delegation**

Replace the entire `at_behavior_channel_class` function (lines 382-405) with:

```c
/* Map active channel name to hu_behavior_input_t.channel_class (policy.h).
 * Knowledge lives in the Channels context — see channels/behavior_class.c. */
static int at_behavior_channel_class(const char *cn, size_t cl) {
    return hu_channel_behavior_class_for_name(cn, cl);
}
```

- [ ] **Step 2: Add the include**

Near the other `#include "human/channels/..."` / agent includes at the top of `agent_turn.c`, add:

```c
#include "human/channels/behavior_class.h"
```

- [ ] **Step 3: Rebuild the production binary (touch first — stale-binary trap)**

Run: `touch src/agent/agent_turn.c && cmake --build build --target human -j8 && cmake --build build --target human_tests -j8`
Expected: see `Linking C executable human` (not just "Built target"), per `.claude/rules/cmake-build-stale-binary.md`.

- [ ] **Step 4: Run the FULL suite (behavior must be unchanged)**

Run: `./build/human_tests`
Expected: 0 failures, 0 ASan errors. The classification contract is identical for all known channels; only unknown-prefix cases changed (now correctly DEFAULT). Watch specifically for any existing agent/behavior test that pinned the old prefix behavior — if one fails, it was pinning the bug (`.claude/rules/tests-that-pin-bugs.md`); update it to assert the exact-match contract.

- [ ] **Step 5: Commit**

```bash
git add src/agent/agent_turn.c
git commit -m "refactor(agent): delegate channel behavior-class to channels context

Removes 14 hardcoded memcmp channel checks from the turn loop.
Adding a channel no longer touches agent core (unblocks M6)."
```

---

## Self-Review

- **Spec coverage:** the 4-class contract (default/voice/chat/email) and all 14 names are in the table (Task 2) and pinned by tests (Task 1). ✓
- **Type consistency:** `hu_channel_behavior_class_for_name` signature identical in header (Task 1), impl (Task 2), test (Task 1), and call site (Task 4). Returns `int`; enum values are the ints. ✓
- **No placeholders:** every step has full code or an exact command. ✓
- **Erosion guard:** Phase 0 adds a rule forbidding new `memcmp(cn, "<channel>"...)` in `src/agent/` so this doesn't regress.

## Follow-up (out of scope, YAGNI)

A vtable method `int (*behavior_class)(void *ctx)` on `hu_channel_t` would let a
channel *object* declare its class directly (purer DDD). Not built now: the turn
loop only has the name string at this site, and the name-keyed table already
moves the knowledge to the right context. Revisit only if a channel needs a
*dynamic* class.
