# Channel Conversation & Chaos Tests Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a channel-level synthetic conversation engine with chaos testing, pressure testing, and real iMessage e2e validation across all 33 human channels.

**Architecture:** Mock inject APIs added to all channels (following MQTT/IMAP/Nostr/Email pattern), a conversation engine that drives Gemini-generated multi-turn dialogues through channel poll→agent→send cycles, composable chaos scenarios, fork-based pressure, and opt-in real iMessage.

**Tech Stack:** C11, libcurl (Gemini API), existing human_core, HU_IS_TEST guards, fork() for pressure.

---

### Task 1: Add mock inject API to iMessage channel

**Files:**

- Modify: `src/channels/imessage.c:23-34` (context struct)
- Modify: `src/channels/imessage.c:103-109` (send HU_IS_TEST branch)
- Modify: `src/channels/imessage.c:378-381` (poll HU_IS_TEST branch)
- Modify: `include/human/channels/imessage.h`
- Test: `tests/test_channel_all.c`

**Step 1: Add mock storage to hu_imessage_ctx_t**

In `src/channels/imessage.c`, add after `size_t sent_ring_idx;` inside the struct:

```c
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
```

**Step 2: Update imessage_send HU_IS_TEST branch to capture last_message**

In `src/channels/imessage.c`, replace the HU_IS_TEST send branch (lines ~103-109) with:

```c
#if HU_IS_TEST
    {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        c->last_message[len] = '\0';
        c->last_message_len = len;
        return HU_OK;
    }
#endif
```

**Step 3: Update hu_imessage_poll HU_IS_TEST branch to return mocks**

In `src/channels/imessage.c`, replace the HU_IS_TEST poll branch (lines ~378-381) with:

```c
#if HU_IS_TEST
    {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)channel_ctx;
        size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, c->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, c->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        c->mock_count = 0;
        return HU_OK;
    }
#endif
```

**Step 4: Add test_inject_mock and test_get_last_message functions**

At the bottom of `src/channels/imessage.c`, before the closing of the file:

```c
#if HU_IS_TEST
hu_error_t hu_imessage_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    return HU_OK;
}

const char *hu_imessage_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
```

**Step 5: Add declarations to imessage.h**

In `include/human/channels/imessage.h`, add before the final `#endif`:

```c
#if HU_IS_TEST
hu_error_t hu_imessage_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len);
const char *hu_imessage_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif
```

**Step 6: Add tests to test_channel_all.c**

Add test functions following the existing MQTT pattern:

```c
#if HU_HAS_IMESSAGE
static void test_imessage_inject_and_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_imessage_test_inject_mock(&ch, "+15559876543", 12, "Hello!", 6);
    HU_ASSERT_EQ(err, HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    err = hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(msgs[0].content, "Hello!");
    hu_imessage_destroy(&ch);
}

static void test_imessage_send_captures_last_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "+15551234567", 12, "Test reply", 10, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &len);
    HU_ASSERT(msg != NULL);
    HU_ASSERT_EQ(len, 10);
    HU_ASSERT_STR_EQ(msg, "Test reply");
    hu_imessage_destroy(&ch);
}
#endif
```

Register in test runner.

**Step 7: Build and verify**

```bash
cmake --build build -j$(sysctl -n hw.ncpu) && ./build/human_tests
```

Expected: all tests pass, including new imessage inject/poll tests.

**Step 8: Commit**

```bash
git add src/channels/imessage.c include/human/channels/imessage.h tests/test_channel_all.c
git commit -m "feat(channels): add mock inject API to iMessage channel"
```

---

### Task 2: Add mock inject APIs to Telegram, Discord, Slack, Signal

Same pattern as Task 1 applied to 4 more channels. Each channel gets:

1. Mock fields in context struct under `#if HU_IS_TEST`
2. `last_message` capture in send's `HU_IS_TEST` branch
3. Mock return in poll's `HU_IS_TEST` branch
4. `hu_<name>_test_inject_mock` + `hu_<name>_test_get_last_message` functions
5. Header declarations
6. Tests in `test_channel_all.c`

**Files:**

- Modify: `src/channels/telegram.c:39-53` — add mock fields
- Modify: `src/channels/telegram.c:481-485` — capture last_message
- Modify: `src/channels/telegram.c:624-629` — return mocks from poll
- Modify: `include/human/channels/telegram.h` — declare test APIs
- Modify: `src/channels/discord.c:17-34` — add mock fields
- Modify: `src/channels/discord.c:94-100` — capture last_message
- Modify: `src/channels/discord.c:359-363` — return mocks from poll
- Modify: `include/human/channels/discord.h` — declare test APIs
- Modify: `src/channels/slack.c:28-42` — add mock fields
- Modify: `src/channels/slack.c:321-325` — capture last_message
- Modify: `src/channels/slack.c:563-567` — return mocks from poll
- Modify: `include/human/channels/slack.h` — declare test APIs
- Modify: `src/channels/signal.c:29-50` — add mock fields
- Modify: `src/channels/signal.c:283-288` — capture last_message
- Modify: `src/channels/signal.c:445-447` — return mocks from poll
- Modify: `include/human/channels/signal.h` — declare test APIs
- Test: `tests/test_channel_all.c` — add inject/poll/send tests for each

For each channel, the mock struct, inject, get_last_message, and poll mock code is identical in structure to the MQTT/iMessage pattern. Only the context type name and create function differ.

**Step 1:** Add mock fields to all 4 context structs.
**Step 2:** Update all 4 send HU_IS_TEST branches to capture last_message.
**Step 3:** Update all 4 poll HU_IS_TEST branches to return mocks.
**Step 4:** Add test_inject_mock + test_get_last_message to all 4 channels.
**Step 5:** Add header declarations.
**Step 6:** Add tests for all 4 channels.
**Step 7:** Build and verify: `cmake --build build && ./build/human_tests`
**Step 8:** Commit: `git commit -m "feat(channels): add mock inject APIs to Telegram, Discord, Slack, Signal"`

---

### Task 3: Add mock inject APIs to Tier 2+3 channels

Same mechanical pattern for remaining channels: WhatsApp, Teams, Matrix, IRC, Line, Facebook, Instagram, Twitter, Google Chat, Google RCS, Lark, DingTalk, Mattermost, OneBot, QQ, Twilio, Web.

**Files:** Each channel's `.c` context struct, send HU_IS_TEST branch, poll HU_IS_TEST branch, and corresponding `.h` header.

**Step 1-6:** Same pattern as Task 2, applied to all remaining channels.
**Step 7:** Build and verify.
**Step 8:** Commit: `git commit -m "feat(channels): add mock inject APIs to all remaining channels"`

---

### Task 4: Create channel_harness.h — types and declarations

**Files:**

- Create: `tests/synthetic/channel_harness.h`

**Step 1: Write the header**

```c
#ifndef HU_CHANNEL_HARNESS_H
#define HU_CHANNEL_HARNESS_H
#include "synthetic_harness.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_chaos_mode {
    HU_CHAOS_NONE = 0,
    HU_CHAOS_MESSAGE = 1,
    HU_CHAOS_INFRA = 2,
    HU_CHAOS_ALL = 3,
} hu_chaos_mode_t;

typedef struct hu_channel_test_config {
    const char *binary_path;
    const char *gemini_api_key;
    const char *gemini_model;
    uint16_t gateway_port;
    int tests_per_channel;
    int concurrency;
    int duration_secs;
    hu_chaos_mode_t chaos;
    const char *regression_dir;
    const char *real_imessage_target;
    const char **channels;
    size_t channel_count;
    bool all_channels;
    bool verbose;
} hu_channel_test_config_t;

typedef struct hu_conversation_turn {
    char user_message[4096];
    char expect_pattern[512];
} hu_conversation_turn_t;

typedef struct hu_conversation_scenario {
    char channel_name[32];
    char session_key[128];
    hu_conversation_turn_t turns[16];
    size_t turn_count;
} hu_conversation_scenario_t;

/* Channel registry — maps name to create/destroy/inject/poll/get_last */
typedef struct hu_channel_test_entry {
    const char *name;
    hu_error_t (*create)(hu_allocator_t *alloc, hu_channel_t *out);
    void (*destroy)(hu_channel_t *ch);
    hu_error_t (*inject)(hu_channel_t *ch, const char *session_key,
                         size_t sk_len, const char *content, size_t c_len);
    hu_error_t (*poll)(void *ctx, hu_allocator_t *alloc,
                       hu_channel_loop_msg_t *msgs, size_t max, size_t *count);
    const char *(*get_last)(hu_channel_t *ch, size_t *out_len);
} hu_channel_test_entry_t;

/* Runners */
hu_error_t hu_channel_run_conversations(hu_allocator_t *alloc,
                                        const hu_channel_test_config_t *cfg,
                                        hu_synth_gemini_ctx_t *gemini,
                                        hu_synth_metrics_t *metrics);

hu_error_t hu_channel_run_chaos(hu_allocator_t *alloc,
                                const hu_channel_test_config_t *cfg,
                                hu_synth_gemini_ctx_t *gemini,
                                hu_synth_metrics_t *metrics);

hu_error_t hu_channel_run_pressure(hu_allocator_t *alloc,
                                   const hu_channel_test_config_t *cfg,
                                   hu_synth_gemini_ctx_t *gemini,
                                   hu_synth_metrics_t *metrics);

hu_error_t hu_channel_run_real_imessage(hu_allocator_t *alloc,
                                        const hu_channel_test_config_t *cfg,
                                        hu_synth_gemini_ctx_t *gemini,
                                        hu_synth_metrics_t *metrics);

/* Channel registry */
const hu_channel_test_entry_t *hu_channel_test_registry(size_t *count);
const hu_channel_test_entry_t *hu_channel_test_find(const char *name);

#define HU_CH_LOG(fmt, ...) fprintf(stderr, "[channel] " fmt "\n", ##__VA_ARGS__)
#define HU_CH_VERBOSE(cfg, fmt, ...) \
    do { if ((cfg)->verbose) fprintf(stderr, "  [v] " fmt "\n", ##__VA_ARGS__); } while (0)

#endif
```

**Step 2: Commit**

```bash
git add tests/synthetic/channel_harness.h
git commit -m "feat(channel-tests): add channel test harness header"
```

---

### Task 5: Create channel_conversation.c — conversation engine

**Files:**

- Create: `tests/synthetic/channel_conversation.c`

Implements `hu_channel_run_conversations`:

1. Builds a Gemini prompt requesting N conversation scenarios for the specified channels
2. Parses the JSON response into `hu_conversation_scenario_t` array
3. For each scenario: looks up channel in registry, creates it, runs turns (inject → poll → send → capture → verify), destroys channel
4. Records per-turn latency and verdict
5. Saves failures to regression dir

The conversation flow per scenario:

- `entry->create(alloc, &ch)`
- For each turn:
  - `entry->inject(&ch, scenario->session_key, sk_len, turn->user_message, msg_len)`
  - `entry->poll(ch.ctx, alloc, msgs, 16, &count)`
  - Verify poll returned the injected message
  - (Agent dispatch is optional — if gateway is running, POST to /v1/chat/completions; otherwise just verify channel roundtrip)
  - `ch.vtable->send(ch.ctx, session_key, sk_len, response, resp_len, NULL, 0)`
  - `entry->get_last(&ch, &len)` — verify response was captured
  - Regex match against `expect_pattern`
- `entry->destroy(&ch)`

**Step 1:** Write the file with Gemini prompt, parsing, and execution loop.
**Step 2:** Build and test: create a simple smoke test.
**Step 3:** Commit.

---

### Task 6: Create channel_chaos.c — chaos scenarios

**Files:**

- Create: `tests/synthetic/channel_chaos.c`

Implements `hu_channel_run_chaos`:

**Message chaos functions** (each takes a channel entry and runs a specific perturbation):

- `chaos_unicode_bomb` — inject zalgo text, RTL, emoji, null bytes; verify no crash
- `chaos_size_extremes` — empty, 1-char, 4095, overflow; verify graceful handling
- `chaos_rapid_fire` — inject 50 messages in tight loop; verify all polled
- `chaos_interleave` — 5 session_keys simultaneously; verify session isolation
- `chaos_malformed` — broken strings, SQL injection; verify no crash
- `chaos_echo_storm` — inject messages matching send output; verify echo suppression

**Infrastructure chaos functions:**

- `chaos_double_start_stop` — call start/stop vtable methods twice
- `chaos_concurrent_send` — fork and send from 2 processes
- `chaos_memory_pressure` — swap in failing allocator, verify graceful OOM handling
- `chaos_gateway_kill_restart` — kill/restart gateway pid between turns

Each function returns a `hu_synth_verdict_t`. The orchestrator runs all applicable chaos types and aggregates metrics.

**Step 1:** Write the file.
**Step 2:** Build and verify.
**Step 3:** Commit.

---

### Task 7: Create channel_pressure.c — pressure testing

**Files:**

- Create: `tests/synthetic/channel_pressure.c`

Implements `hu_channel_run_pressure`:

- Forks N workers (from `cfg->concurrency`)
- Each worker: creates a channel, runs conversation turns in a loop for `cfg->duration_secs`
- Metrics per worker piped to parent: total turns, pass/fail, latencies
- Parent aggregates: conversations/sec, avg turn latency, error rate

Pattern follows `tests/synthetic/synthetic_pressure.c` (existing).

**Step 1:** Write the file.
**Step 2:** Build and verify.
**Step 3:** Commit.

---

### Task 8: Create channel_imessage_real.c — real iMessage e2e

**Files:**

- Create: `tests/synthetic/channel_imessage_real.c`

Implements `hu_channel_run_real_imessage`:

- Only compiles on `__APPLE__` with `HU_ENABLE_SQLITE`
- Checks Messages.app is running (via `pgrep Messages`)
- Creates real iMessage channel with `cfg->real_imessage_target`
- Gemini generates a conversation starter
- Sends via real `imessage_send` (AppleScript)
- Polls `chat.db` for response with 30-second timeout
- Reports send success, poll success, round-trip latency
- Non-Apple: returns `HU_ERR_NOT_SUPPORTED`

**Step 1:** Write the file.
**Step 2:** Manual test on macOS (requires Messages.app and a target).
**Step 3:** Commit.

---

### Task 9: Create channel_main.c — orchestrator

**Files:**

- Create: `tests/synthetic/channel_main.c`

The `main()` function:

1. Parse CLI args: `--binary`, `--channels`, `--count`, `--chaos`, `--concurrency`, `--duration`, `--real-imessage`, `--regression-dir`, `--verbose`, `--help`
2. Get `GEMINI_API_KEY` from env
3. Initialize Gemini context
4. Build channel registry
5. Start gateway if needed (for agent dispatch tests)
6. Run conversation engine for each specified channel
7. Run chaos layer if `--chaos != none`
8. Run pressure if `--concurrency > 0 && --duration > 0`
9. Run real iMessage if `--real-imessage` specified
10. Print final report
11. Cleanup

**Step 1:** Write the file.
**Step 2:** Build and verify `--help` output.
**Step 3:** Commit.

---

### Task 10: Build channel registry — connect all channels to test entries

**Files:**

- Create: `tests/synthetic/channel_registry.c`

Static array mapping channel name → create/destroy/inject/poll/get*last function pointers. Uses `HU_HAS*\*` guards so only compiled-in channels are registered.

```c
static hu_channel_test_entry_t s_registry[] = {
#if HU_HAS_IMESSAGE
    { "imessage", imessage_test_create, imessage_test_destroy,
      hu_imessage_test_inject_mock, hu_imessage_poll, hu_imessage_test_get_last_message },
#endif
#if HU_HAS_TELEGRAM
    { "telegram", telegram_test_create, telegram_test_destroy,
      hu_telegram_test_inject_mock, hu_telegram_poll, hu_telegram_test_get_last_message },
#endif
    /* ... all channels ... */
};
```

Each `<name>_test_create` wrapper calls `hu_<name>_create` with neutral test config.

**Step 1:** Write the file.
**Step 2:** Build and verify.
**Step 3:** Commit.

---

### Task 11: CMake integration

**Files:**

- Modify: `CMakeLists.txt:1168+`

Add after the `HU_ENABLE_SYNTHETIC` block:

```cmake
option(HU_ENABLE_CHANNEL_TESTS "Build channel conversation+chaos test harness" OFF)
if(HU_ENABLE_CHANNEL_TESTS)
    file(GLOB HU_CHANNEL_TEST_SOURCES tests/synthetic/channel_*.c)
    list(APPEND HU_CHANNEL_TEST_SOURCES
        tests/synthetic/synthetic_gemini.c
        tests/synthetic/synthetic_regression.c)
    add_executable(human_channel_tests ${HU_CHANNEL_TEST_SOURCES})
    target_link_libraries(human_channel_tests human_core)
    target_include_directories(human_channel_tests PRIVATE
        ${HU_ROOT}/include ${HU_ROOT}/src ${HU_ROOT}/tests/synthetic)
    target_compile_definitions(human_channel_tests PRIVATE
        HU_ENABLE_CURL=1 HU_IS_TEST=1)
    target_compile_options(human_channel_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wformat-security
        -Wno-gnu-zero-variadic-macro-arguments)
    if(UNIX AND NOT WIN32)
        target_compile_definitions(human_channel_tests PRIVATE HU_GATEWAY_POSIX=1)
    endif()
    if(HU_ENABLE_SQLITE)
        target_compile_definitions(human_channel_tests PRIVATE HU_ENABLE_SQLITE=1)
        if(SQLITE3_FOUND)
            target_include_directories(human_channel_tests PRIVATE ${SQLITE3_INCLUDE_DIRS})
        endif()
    endif()
endif()
```

Key: `HU_IS_TEST=1` is defined so mock inject APIs are compiled in.

**Step 1:** Add the CMake target.
**Step 2:** Build: `cmake -B build -DSC_ENABLE_CHANNEL_TESTS=ON -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SQLITE=ON && cmake --build build`
**Step 3:** Verify `human_channel_tests --help` works.
**Step 4:** Commit.

---

### Task 12: Full integration test — run and validate

**Step 1:** Build everything:

```bash
cmake -B build -DSC_ENABLE_CHANNEL_TESTS=ON -DSC_ENABLE_SYNTHETIC=ON \
      -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SQLITE=ON
cmake --build build -j$(sysctl -n hw.ncpu)
```

**Step 2:** Run existing test suite (no regressions):

```bash
./build/human_tests
```

**Step 3:** Run channel conversations (Tier 1 channels):

```bash
GEMINI_API_KEY="..." ./build/human_channel_tests \
    --binary ./build/human \
    --channels imessage,telegram,discord,slack,signal \
    --count 3 --verbose
```

**Step 4:** Run with chaos:

```bash
GEMINI_API_KEY="..." ./build/human_channel_tests \
    --channels imessage,telegram --count 3 --chaos all --verbose
```

**Step 5:** Run pressure:

```bash
GEMINI_API_KEY="..." ./build/human_channel_tests \
    --channels imessage,telegram,discord,slack,signal \
    --concurrency 4 --duration 10 --verbose
```

**Step 6:** Run all channels:

```bash
GEMINI_API_KEY="..." ./build/human_channel_tests \
    --channels all --count 2 --chaos message --verbose
```

**Step 7:** Commit all fixes from integration testing.
