---
title: Native Messages Voice Delivery Implementation Plan (W3)
description: Deliver voice replies by driving Messages' own Record/Stop/Send with Cartesia audio fed through a BlackHole input.
category: plans
---

# Native Messages Voice Delivery Implementation Plan (W3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When `HU_VOICE_DELIVERY=messages`, an iMessage voice reply is recorded by Messages itself (a genuine `is_audio_message=1` memo) instead of being sent as a file attachment; every failure before Send falls back to today's attachment path, and the user's mic and screen are always restored.

**Architecture:** Three layers. (1) Pure policy + orchestrator (`imessage_voice_record.c`, all platforms) decides *whether* and *in what order* to act, against a `hu_voice_record_port_t` vtable — fully unit-tested with a fake port. (2) A macOS port (`imessage_voice_record_macos.c`, Apple production only) implements the port with CoreAudio (input device switch + read-back, mic-in-use), AudioQueue (playback to BlackHole), AX (press by label), IOKit (idle time) and two chat.db helpers that live in `imessage.c` (which already includes sqlite). (3) `imessage_send` routes a memo-shaped send (empty text + one audio file) through the orchestrator when the env gate says so.

**Tech Stack:** C11; Apple frameworks CoreAudio, AudioToolbox (new), ApplicationServices, IOKit, CoreFoundation (existing); objc runtime (existing pattern `imessage.c:3883`).

**Spec:** `docs/superpowers/specs/2026-09-26-voice-memo-realism-design.md` (section W3). Spike evidence: chat.db row 73060 (`is_audio_message=1`, Opus 24 kHz), AX labels on macOS 26.6 `Record audio` → (`Stop`, `Cancel audio recording`) → (`play`, `Send`, `Cancel audio recording`).

## Global Constraints

- C11, `-Wall -Wextra -Wpedantic -Werror`; ASan clean; free every allocation.
- Tests: no hardware, no AX, no audio, no network, no process spawning. All real-world effects live behind the port and are replaced by a fake in tests.
- Default is OFF: `HU_VOICE_DELIVERY` unset/unknown ⇒ `attachment` (today's behavior, byte-for-byte). Values: `attachment`, `shadow` (run preflight, log the decision, still send the attachment), `messages` (live).
- Gate comment at the activation site: activation of `messages` as a default is gated on the W5 "real or clone?" measurement.
- Never log message text or the recipient handle; log block reasons and stage names only.
- Restore is unconditional: the default input is set back to the **configured real mic** (`HU_VOICE_REAL_INPUT`, e.g. `Shure MV7`) — never "whatever was there before" (spike bug) — and read back; failure to restore is logged at error level.
- No new `#include <sqlite3.h>` outside `imessage.c` (sqlite-includer ratchet). No new cross-channel include except the new shared header, added to the edge-isolation exemption list.
- New `src/channels/*.c` files, not `src/` root. Work only in `/Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism` with absolute paths.
- **Every commit keeps the dead-strip counters flat** (`scripts/check-dead-strip-ratchet.sh`, pre-commit): a new object file the daemon never loads raises counter A and the commit is refused. So each layer gets a real product caller in the task that creates it — Task 1 wires `hu_voice_record_route` into `imessage_send` (log-only), Task 3 makes the SHADOW branch call the macOS port, Task 4 adds the live branch. Never `--no-verify`.

## Review Focus

- **User is on a call** (real mic in use by another process) ⇒ no switch, attachment fallback. Pinned in Task 2 (`preflight` → `MIC_BUSY` ⇒ orchestrator never calls `set_input`).
- **Failure after Record pressed but before Send** (playback error, `Stop` never appears) ⇒ `Cancel audio recording` pressed, input restored, attachment fallback. Pinned in Task 2.
- **Send pressed but chat.db never shows the row** ⇒ do NOT re-send as attachment (duplicate risk); return OK-unverified and log. Pinned in Task 2.
- **Input read-back mismatch** (switch silently didn't take) ⇒ abort before pressing Record, restore, fallback. Pinned in Task 2.
- **A normal text send or a non-audio attachment** must never enter the record path. Pinned in Task 1 (`hu_voice_record_is_memo_send`).

---

### Task 1: Policy predicates (pure)

**Files:**
- Create: `include/human/channels/imessage_voice_record.h`
- Create: `src/channels/imessage_voice_record.c`
- Test: `tests/test_imessage_voice_record.c`
- Modify: `CMakeLists.txt` (add source to the unconditional core list next to `src/channels/imessage_send_observer.c`; add test next to `tests/test_imessage_send_observer.c` at ~line 3712), `tests/test_main.c` (declare + call `run_imessage_voice_record_tests()` next to `run_imessage_send_observer_tests`), `scripts/check-edge-context-isolation.sh` (add `imessage_voice_record` to both exemption `case` lists, lines ~38 and ~56, and the comment at ~line 19), `.claude/rules/edge-context-isolation.md` (add `imessage_voice_record` to the exempt-header sentence), `src/channels/imessage.c` (`imessage_send` production branch, just before the `imsg_media:` attachment loop at ~line 2518: compute the route and, when it is not ATTACHMENT, log `voice delivery route=<shadow|record> (not yet active)`; behavior unchanged — this is the product caller that keeps the object loaded).

**Interfaces — Produces:**

```c
typedef enum {
    HU_VOICE_DELIVERY_ATTACHMENT = 0,
    HU_VOICE_DELIVERY_SHADOW,
    HU_VOICE_DELIVERY_MESSAGES,
} hu_voice_delivery_mode_t;

hu_voice_delivery_mode_t hu_voice_delivery_mode_parse(const char *s); /* NULL/unknown -> ATTACHMENT */
bool hu_voice_record_is_memo_send(size_t message_len, const char *const *media, size_t media_count);

typedef struct {
    bool ax_trusted;
    bool messages_running;
    bool blackhole_present;
    bool real_mic_configured; /* HU_VOICE_REAL_INPUT set and non-empty */
    bool real_mic_present;
    bool real_mic_busy;       /* kAudioDevicePropertyDeviceIsRunningSomewhere */
    double user_idle_sec;
    double min_idle_sec;
} hu_voice_record_facts_t;

typedef enum {
    HU_VREC_OK = 0,
    HU_VREC_NO_AX,
    HU_VREC_NO_MESSAGES,
    HU_VREC_NO_BLACKHOLE,
    HU_VREC_NO_REAL_MIC,
    HU_VREC_MIC_BUSY,
    HU_VREC_USER_ACTIVE,
} hu_voice_record_block_t;

hu_voice_record_block_t hu_voice_record_preflight(const hu_voice_record_facts_t *f);
const char *hu_voice_record_block_name(hu_voice_record_block_t b);

typedef struct { uint32_t lead_in_ms; uint32_t tail_ms; } hu_voice_record_timing_t;
void hu_voice_record_timing(uint32_t seed, hu_voice_record_timing_t *out); /* lead 350..700, tail 500..900 */

#define HU_VREC_LABEL_RECORD "Record audio"
#define HU_VREC_LABEL_STOP   "Stop"
#define HU_VREC_LABEL_SEND   "Send"
#define HU_VREC_LABEL_CANCEL "Cancel audio recording"
#define HU_VREC_BLACKHOLE_NAME "BlackHole 2ch"

/* Pure routing decision used by imessage_send. */
typedef enum { HU_VREC_ROUTE_ATTACHMENT = 0, HU_VREC_ROUTE_SHADOW, HU_VREC_ROUTE_RECORD } hu_voice_record_route_t;
hu_voice_record_route_t hu_voice_record_route(hu_voice_delivery_mode_t mode, size_t message_len,
                                              const char *const *media, size_t media_count);
```

Route rule: `!hu_voice_record_is_memo_send(...) || mode == ATTACHMENT` ⇒ ATTACHMENT; `SHADOW` ⇒ SHADOW; `MESSAGES` ⇒ RECORD.

- [ ] **Step 1: Write failing tests** in `tests/test_imessage_voice_record.c`:

```c
#include "human/channels/imessage_voice_record.h"
#include "test_framework.h"
#include <string.h>

static hu_voice_record_facts_t all_ok(void) {
    hu_voice_record_facts_t f = {.ax_trusted = true, .messages_running = true,
                                 .blackhole_present = true, .real_mic_configured = true,
                                 .real_mic_present = true, .real_mic_busy = false,
                                 .user_idle_sec = 120.0, .min_idle_sec = 20.0};
    return f;
}

static void test_vrec_mode_parse_defaults_to_attachment(void) {
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse(NULL), HU_VOICE_DELIVERY_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse(""), HU_VOICE_DELIVERY_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse("live"), HU_VOICE_DELIVERY_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse("shadow"), HU_VOICE_DELIVERY_SHADOW);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse("messages"), HU_VOICE_DELIVERY_MESSAGES);
}

static void test_vrec_memo_send_requires_empty_text_and_one_audio(void) {
    const char *caf[] = {"/tmp/a/Audio Message.caf"};
    const char *mp3[] = {"/tmp/human_dtts_1.mp3"};
    const char *png[] = {"/tmp/pic.png"};
    const char *two[] = {"/tmp/a.caf", "/tmp/b.caf"};
    HU_ASSERT_TRUE(hu_voice_record_is_memo_send(0, caf, 1));
    HU_ASSERT_TRUE(hu_voice_record_is_memo_send(0, mp3, 1));
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(5, caf, 1));   /* text + audio */
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(0, png, 1));   /* not audio */
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(0, two, 2));   /* two files */
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(0, NULL, 0));  /* plain text */
}

static void test_vrec_preflight_ok_when_all_facts_hold(void) {
    hu_voice_record_facts_t f = all_ok();
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_OK);
}

static void test_vrec_preflight_blocks_each_fact(void) {
    hu_voice_record_facts_t f;
    f = all_ok(); f.ax_trusted = false;          HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_AX);
    f = all_ok(); f.messages_running = false;    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_MESSAGES);
    f = all_ok(); f.blackhole_present = false;   HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_BLACKHOLE);
    f = all_ok(); f.real_mic_configured = false; HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_REAL_MIC);
    f = all_ok(); f.real_mic_present = false;    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_REAL_MIC);
    f = all_ok(); f.real_mic_busy = true;        HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_MIC_BUSY);
    f = all_ok(); f.user_idle_sec = 5.0;         HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_USER_ACTIVE);
    HU_ASSERT_EQ(hu_voice_record_preflight(NULL), HU_VREC_NO_AX); /* fail closed */
}

static void test_vrec_timing_stays_in_human_ranges(void) {
    for (uint32_t s = 0; s < 500; s++) {
        hu_voice_record_timing_t t;
        hu_voice_record_timing(s * 2654435761u, &t);
        HU_ASSERT_TRUE(t.lead_in_ms >= 350 && t.lead_in_ms <= 700);
        HU_ASSERT_TRUE(t.tail_ms >= 500 && t.tail_ms <= 900);
    }
}

static void test_vrec_block_names_are_distinct(void) {
    HU_ASSERT_STR_EQ(hu_voice_record_block_name(HU_VREC_OK), "ok");
    HU_ASSERT_STR_EQ(hu_voice_record_block_name(HU_VREC_MIC_BUSY), "mic_busy");
    HU_ASSERT_STR_EQ(hu_voice_record_block_name(HU_VREC_USER_ACTIVE), "user_active");
}

static void test_vrec_route_only_memo_sends_in_non_attachment_modes(void) {
    const char *caf[] = {"/tmp/Audio Message.caf"};
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_ATTACHMENT, 0, caf, 1), HU_VREC_ROUTE_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_SHADOW, 0, caf, 1), HU_VREC_ROUTE_SHADOW);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_MESSAGES, 0, caf, 1), HU_VREC_ROUTE_RECORD);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_MESSAGES, 4, caf, 1), HU_VREC_ROUTE_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_MESSAGES, 4, NULL, 0), HU_VREC_ROUTE_ATTACHMENT);
}

void run_imessage_voice_record_tests(void) {
    HU_TEST_SUITE("imessage voice record");
    HU_RUN_TEST(test_vrec_mode_parse_defaults_to_attachment);
    HU_RUN_TEST(test_vrec_memo_send_requires_empty_text_and_one_audio);
    HU_RUN_TEST(test_vrec_preflight_ok_when_all_facts_hold);
    HU_RUN_TEST(test_vrec_preflight_blocks_each_fact);
    HU_RUN_TEST(test_vrec_timing_stays_in_human_ranges);
    HU_RUN_TEST(test_vrec_block_names_are_distinct);
    HU_RUN_TEST(test_vrec_route_only_memo_sends_in_non_attachment_modes);
}
```

- [ ] **Step 2:** Register test + source in CMake/test_main (Files list), build: `cmake --build <W>/build --target human_tests -j10 2>&1 | grep error:` — Expected: undefined/undeclared `hu_voice_*` errors.

- [ ] **Step 3: Implement** the header (declarations above, include guards `HU_CHANNELS_IMESSAGE_VOICE_RECORD_H`, `<stdbool.h> <stddef.h> <stdint.h>`, `human/core/error.h`) and `src/channels/imessage_voice_record.c`:

```c
#include "human/channels/imessage_voice_record.h"
#include <string.h>
#include <strings.h>

hu_voice_delivery_mode_t hu_voice_delivery_mode_parse(const char *s) {
    if (s && strcmp(s, "messages") == 0)
        return HU_VOICE_DELIVERY_MESSAGES;
    if (s && strcmp(s, "shadow") == 0)
        return HU_VOICE_DELIVERY_SHADOW;
    return HU_VOICE_DELIVERY_ATTACHMENT;
}

static bool has_audio_ext(const char *p) {
    static const char *const exts[] = {".caf", ".m4a", ".mp3", ".wav"};
    size_t n = strlen(p);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t e = strlen(exts[i]);
        if (n > e && strcasecmp(p + n - e, exts[i]) == 0)
            return true;
    }
    return false;
}

bool hu_voice_record_is_memo_send(size_t message_len, const char *const *media, size_t media_count) {
    return message_len == 0 && media && media_count == 1 && media[0] && media[0][0] == '/' &&
           has_audio_ext(media[0]);
}

hu_voice_record_block_t hu_voice_record_preflight(const hu_voice_record_facts_t *f) {
    if (!f || !f->ax_trusted)
        return HU_VREC_NO_AX;
    if (!f->messages_running)
        return HU_VREC_NO_MESSAGES;
    if (!f->blackhole_present)
        return HU_VREC_NO_BLACKHOLE;
    if (!f->real_mic_configured || !f->real_mic_present)
        return HU_VREC_NO_REAL_MIC;
    if (f->real_mic_busy)
        return HU_VREC_MIC_BUSY;
    if (f->user_idle_sec < f->min_idle_sec)
        return HU_VREC_USER_ACTIVE;
    return HU_VREC_OK;
}

const char *hu_voice_record_block_name(hu_voice_record_block_t b) {
    switch (b) {
    case HU_VREC_OK: return "ok";
    case HU_VREC_NO_AX: return "no_ax";
    case HU_VREC_NO_MESSAGES: return "no_messages";
    case HU_VREC_NO_BLACKHOLE: return "no_blackhole";
    case HU_VREC_NO_REAL_MIC: return "no_real_mic";
    case HU_VREC_MIC_BUSY: return "mic_busy";
    case HU_VREC_USER_ACTIVE: return "user_active";
    }
    return "unknown";
}

void hu_voice_record_timing(uint32_t seed, hu_voice_record_timing_t *out) {
    if (!out)
        return;
    uint32_t x = seed ? seed : 0x9e3779b9u; /* xorshift32 */
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    out->lead_in_ms = 350u + x % 351u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    out->tail_ms = 500u + x % 401u;
}

hu_voice_record_route_t hu_voice_record_route(hu_voice_delivery_mode_t mode, size_t message_len,
                                              const char *const *media, size_t media_count) {
    if (mode == HU_VOICE_DELIVERY_ATTACHMENT ||
        !hu_voice_record_is_memo_send(message_len, media, media_count))
        return HU_VREC_ROUTE_ATTACHMENT;
    return mode == HU_VOICE_DELIVERY_SHADOW ? HU_VREC_ROUTE_SHADOW : HU_VREC_ROUTE_RECORD;
}
```

Then the log-only product caller in `imessage_send` (production branch, before `imsg_media:`):

```c
    {
        hu_voice_record_route_t vroute = hu_voice_record_route(
            hu_voice_delivery_mode_parse(getenv("HU_VOICE_DELIVERY")), message_len, media, media_count);
        if (vroute != HU_VREC_ROUTE_ATTACHMENT)
            hu_log_info("imessage", NULL, "voice delivery route=%s (not yet active)",
                        vroute == HU_VREC_ROUTE_SHADOW ? "shadow" : "record");
    }
```

- [ ] **Step 4:** Build `human` + `human_tests`; `./build/human_tests --filter=vrec` — Expected: 7 PASS. Run `bash scripts/check-edge-context-isolation.sh` — Expected: pass. Dead-strip check on commit must show A and B unchanged.
- [ ] **Step 5: Mutation check:** make `hu_voice_record_preflight` ignore `real_mic_busy`; expect `test_vrec_preflight_blocks_each_fact` FAIL; restore.
- [ ] **Step 6: Commit** `feat(voice): pure policy for native Messages voice delivery`.

---

### Task 2: Orchestrator over a port (pure, fake-port tested)

**Files:**
- Modify: `include/human/channels/imessage_voice_record.h`, `src/channels/imessage_voice_record.c`
- Test: `tests/test_imessage_voice_record.c`

**Interfaces — Consumes:** Task 1. **Produces:**

```c
typedef struct hu_voice_record_port {
    void *ctx;
    hu_error_t (*gather_facts)(void *ctx, const char *real_mic, hu_voice_record_facts_t *out);
    hu_error_t (*set_input)(void *ctx, const char *device_name); /* sets default input */
    hu_error_t (*get_input)(void *ctx, char *buf, size_t cap);   /* reads default input name */
    hu_error_t (*remember_ui)(void *ctx);
    void (*restore_ui)(void *ctx);
    hu_error_t (*open_chat)(void *ctx, const char *handle, size_t handle_len);
    hu_error_t (*press)(void *ctx, const char *label);
    bool (*wait_label)(void *ctx, const char *label, uint32_t timeout_ms);
    hu_error_t (*playback_prepare)(void *ctx, const char *audio_path);
    hu_error_t (*playback_run)(void *ctx); /* blocks until the clip has fully played */
    void (*playback_dispose)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    int64_t (*max_rowid)(void *ctx); /* -1 unknown */
    bool (*audio_row_after)(void *ctx, const char *handle, size_t handle_len, int64_t after_rowid,
                            uint32_t timeout_ms);
} hu_voice_record_port_t;

typedef struct {
    const char *handle; size_t handle_len;
    const char *audio_path;
    const char *real_mic;      /* HU_VOICE_REAL_INPUT */
    double min_idle_sec;       /* default 20 */
    uint32_t seed;
} hu_voice_record_request_t;

typedef enum {
    HU_VREC_STAGE_NONE = 0, HU_VREC_STAGE_PREFLIGHT, HU_VREC_STAGE_INPUT, HU_VREC_STAGE_OPEN,
    HU_VREC_STAGE_RECORD, HU_VREC_STAGE_PLAY, HU_VREC_STAGE_STOP, HU_VREC_STAGE_SENT,
} hu_voice_record_stage_t;

typedef struct {
    hu_voice_record_block_t block;
    hu_voice_record_stage_t stage;  /* furthest stage reached */
    bool verified;                  /* chat.db row confirmed */
    bool restored;                  /* input read back == real_mic after restore */
    int64_t prior_max_rowid;
} hu_voice_record_result_t;

/* HU_OK: Send was pressed (check result.verified). HU_ERR_NOT_SUPPORTED: preflight blocked,
 * nothing touched. HU_ERR_IO: failed before Send; everything restored; caller should fall
 * back to the attachment send. */
hu_error_t hu_voice_record_send(const hu_voice_record_port_t *port,
                                const hu_voice_record_request_t *req,
                                hu_voice_record_result_t *out);
```

**Order the orchestrator must follow** (each step's failure jumps to the restore block):
preflight (gather_facts → `hu_voice_record_preflight`; blocked ⇒ return NOT_SUPPORTED with **no other port calls**) → `remember_ui` → `max_rowid` → `playback_prepare` → `set_input(BlackHole)` then `get_input` must equal `HU_VREC_BLACKHOLE_NAME` → `open_chat` → `wait_label(Record, 3000)` → `press(Record)` → `wait_label(Stop, 3000)` → `sleep_ms(lead_in)` → `playback_run` → `sleep_ms(tail)` → `press(Stop)` → `wait_label(Send, 3000)` → `press(Send)` [stage SENT] → restore block → `audio_row_after(prior, 10000)` sets `verified`.
**Restore block (always, every path after preflight):** if stage is RECORD/PLAY/STOP and Send was not pressed ⇒ `press(Cancel)`; `playback_dispose`; `set_input(real_mic)` then `get_input` == real_mic sets `restored`; `restore_ui`.

- [ ] **Step 1: Write failing tests** — a fake port that appends each call to a trace string and can be told to fail at a named call:

```c
typedef struct {
    char trace[1024];
    const char *fail_at;       /* call name that returns an error / false */
    hu_voice_record_facts_t facts;
    char input[64];
    bool row_found;
} fake_port_t;

static void fp_log(fake_port_t *f, const char *s) {
    strncat(f->trace, s, sizeof(f->trace) - strlen(f->trace) - 2);
    strncat(f->trace, ",", sizeof(f->trace) - strlen(f->trace) - 1);
}
static bool fp_fail(fake_port_t *f, const char *s) { return f->fail_at && strcmp(f->fail_at, s) == 0; }

static hu_error_t fp_gather(void *c, const char *m, hu_voice_record_facts_t *o) { (void)m; fake_port_t *f = c; fp_log(f, "facts"); *o = f->facts; return HU_OK; }
static hu_error_t fp_set_input(void *c, const char *d) { fake_port_t *f = c; fp_log(f, strcmp(d, HU_VREC_BLACKHOLE_NAME) == 0 ? "in:bh" : "in:real"); if (!fp_fail(f, "set_input")) snprintf(f->input, sizeof(f->input), "%s", d); return HU_OK; }
static hu_error_t fp_get_input(void *c, char *b, size_t n) { fake_port_t *f = c; snprintf(b, n, "%s", f->input); return HU_OK; }
static hu_error_t fp_remember(void *c) { fp_log(c, "remember"); return HU_OK; }
static void fp_restore_ui(void *c) { fp_log(c, "restore_ui"); }
static hu_error_t fp_open(void *c, const char *h, size_t n) { (void)h; (void)n; fp_log(c, "open"); return HU_OK; }
static hu_error_t fp_press(void *c, const char *l) {
    fake_port_t *f = c; char b[48]; snprintf(b, sizeof(b), "press:%s", l); fp_log(f, b);
    return HU_OK;
}
static bool fp_wait(void *c, const char *l, uint32_t t) {
    (void)t; fake_port_t *f = c; char b[48]; snprintf(b, sizeof(b), "wait:%s", l);
    return !fp_fail(f, b);
}
static hu_error_t fp_prep(void *c, const char *p) { (void)p; fp_log(c, "prep"); return HU_OK; }
static hu_error_t fp_run(void *c) { fake_port_t *f = c; fp_log(f, "play"); return fp_fail(f, "play") ? HU_ERR_IO : HU_OK; }
static void fp_dispose(void *c) { fp_log(c, "dispose"); }
static void fp_sleep(void *c, uint32_t ms) { (void)c; (void)ms; }
static int64_t fp_rowid(void *c) { (void)c; return 73000; }
static bool fp_row(void *c, const char *h, size_t n, int64_t a, uint32_t t) { (void)h; (void)n; (void)a; (void)t; return ((fake_port_t *)c)->row_found; }

static hu_voice_record_port_t fake_port(fake_port_t *f) {
    hu_voice_record_port_t p = {f, fp_gather, fp_set_input, fp_get_input, fp_remember, fp_restore_ui,
                                fp_open, fp_press, fp_wait, fp_prep, fp_run, fp_dispose, fp_sleep,
                                fp_rowid, fp_row};
    return p;
}

static hu_voice_record_request_t req_ok(void) {
    hu_voice_record_request_t r = {"+15550000001", 12, "/tmp/a.caf", "Shure MV7", 20.0, 7};
    return r;
}

static void test_vrec_send_happy_path_order(void) {
    fake_port_t f = {0}; f.facts = all_ok(); f.row_found = true; snprintf(f.input, sizeof(f.input), "Shure MV7");
    hu_voice_record_port_t p = fake_port(&f); hu_voice_record_request_t r = req_ok(); hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_OK);
    HU_ASSERT_STR_EQ(f.trace, "facts,remember,prep,in:bh,open,press:Record audio,play,"
                              "press:Stop,press:Send,dispose,in:real,restore_ui,");
    HU_ASSERT_TRUE(res.verified);
    HU_ASSERT_TRUE(res.restored);
    HU_ASSERT_EQ(res.stage, HU_VREC_STAGE_SENT);
    HU_ASSERT_EQ(res.prior_max_rowid, 73000);
}

static void test_vrec_send_mic_busy_touches_nothing(void) {
    fake_port_t f = {0}; f.facts = all_ok(); f.facts.real_mic_busy = true;
    hu_voice_record_port_t p = fake_port(&f); hu_voice_record_request_t r = req_ok(); hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_STR_EQ(f.trace, "facts,");
    HU_ASSERT_EQ(res.block, HU_VREC_MIC_BUSY);
}

static void test_vrec_send_input_readback_mismatch_aborts_before_record(void) {
    fake_port_t f = {0}; f.facts = all_ok(); f.fail_at = "set_input"; snprintf(f.input, sizeof(f.input), "Shure MV7");
    hu_voice_record_port_t p = fake_port(&f); hu_voice_record_request_t r = req_ok(); hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_ERR_IO);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Record audio") == NULL);
    HU_ASSERT_TRUE(strstr(f.trace, "restore_ui") != NULL);
}

static void test_vrec_send_playback_failure_cancels_and_restores(void) {
    fake_port_t f = {0}; f.facts = all_ok(); f.fail_at = "play"; snprintf(f.input, sizeof(f.input), "Shure MV7");
    hu_voice_record_port_t p = fake_port(&f); hu_voice_record_request_t r = req_ok(); hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_ERR_IO);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Cancel audio recording") != NULL);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Send") == NULL);
    HU_ASSERT_TRUE(strstr(f.trace, "in:real,restore_ui,") != NULL);
    HU_ASSERT_TRUE(res.restored);
}

static void test_vrec_send_missing_send_button_cancels(void) {
    fake_port_t f = {0}; f.facts = all_ok(); f.fail_at = "wait:Send"; snprintf(f.input, sizeof(f.input), "Shure MV7");
    hu_voice_record_port_t p = fake_port(&f); hu_voice_record_request_t r = req_ok(); hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_ERR_IO);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Cancel audio recording") != NULL);
}

static void test_vrec_send_unverified_row_is_ok_not_resent(void) {
    fake_port_t f = {0}; f.facts = all_ok(); f.row_found = false; snprintf(f.input, sizeof(f.input), "Shure MV7");
    hu_voice_record_port_t p = fake_port(&f); hu_voice_record_request_t r = req_ok(); hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_OK); /* caller must NOT fall back */
    HU_ASSERT_FALSE(res.verified);
}

static void test_vrec_send_restore_readback_failure_is_reported(void) {
    /* set_input "fails" silently: input stays on whatever it was; start it at BlackHole
     * so restore cannot bring it back to the real mic. */
    fake_port_t f = {0}; f.facts = all_ok(); f.row_found = true; f.fail_at = "set_input";
    snprintf(f.input, sizeof(f.input), HU_VREC_BLACKHOLE_NAME);
    hu_voice_record_port_t p = fake_port(&f); hu_voice_record_request_t r = req_ok(); hu_voice_record_result_t res;
    (void)hu_voice_record_send(&p, &r, &res);
    HU_ASSERT_FALSE(res.restored);
}
```

(Register all seven with `HU_RUN_TEST` in `run_imessage_voice_record_tests`. The fake `wait_label` returns true for every label unless named in `fail_at`, so the happy trace has no `wait:` entries.)

- [ ] **Step 2:** Build — Expected: undeclared `hu_voice_record_send` / unknown type errors.
- [ ] **Step 3: Implement** `hu_voice_record_send` in `src/channels/imessage_voice_record.c`:

```c
static bool input_is(const hu_voice_record_port_t *p, const char *want) {
    char cur[128] = {0};
    return p->get_input(p->ctx, cur, sizeof(cur)) == HU_OK && strcmp(cur, want) == 0;
}

hu_error_t hu_voice_record_send(const hu_voice_record_port_t *p, const hu_voice_record_request_t *req,
                                hu_voice_record_result_t *out) {
    if (!p || !req || !out || !req->audio_path || !req->handle || !req->real_mic)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->prior_max_rowid = -1;

    hu_voice_record_facts_t facts;
    memset(&facts, 0, sizeof(facts));
    if (p->gather_facts(p->ctx, req->real_mic, &facts) != HU_OK)
        facts.ax_trusted = false;
    facts.min_idle_sec = req->min_idle_sec;
    out->stage = HU_VREC_STAGE_PREFLIGHT;
    out->block = hu_voice_record_preflight(&facts);
    if (out->block != HU_VREC_OK)
        return HU_ERR_NOT_SUPPORTED;

    hu_voice_record_timing_t tm;
    hu_voice_record_timing(req->seed, &tm);
    hu_error_t rc = HU_ERR_IO;
    bool sent = false;

    if (p->remember_ui(p->ctx) != HU_OK)
        goto restore;
    out->prior_max_rowid = p->max_rowid(p->ctx);
    if (p->playback_prepare(p->ctx, req->audio_path) != HU_OK)
        goto restore;
    out->stage = HU_VREC_STAGE_INPUT;
    if (p->set_input(p->ctx, HU_VREC_BLACKHOLE_NAME) != HU_OK || !input_is(p, HU_VREC_BLACKHOLE_NAME))
        goto restore;
    out->stage = HU_VREC_STAGE_OPEN;
    if (p->open_chat(p->ctx, req->handle, req->handle_len) != HU_OK ||
        !p->wait_label(p->ctx, HU_VREC_LABEL_RECORD, 3000))
        goto restore;
    if (p->press(p->ctx, HU_VREC_LABEL_RECORD) != HU_OK)
        goto restore;
    out->stage = HU_VREC_STAGE_RECORD;
    if (!p->wait_label(p->ctx, HU_VREC_LABEL_STOP, 3000))
        goto restore;
    p->sleep_ms(p->ctx, tm.lead_in_ms);
    out->stage = HU_VREC_STAGE_PLAY;
    if (p->playback_run(p->ctx) != HU_OK)
        goto restore;
    p->sleep_ms(p->ctx, tm.tail_ms);
    out->stage = HU_VREC_STAGE_STOP;
    if (p->press(p->ctx, HU_VREC_LABEL_STOP) != HU_OK || !p->wait_label(p->ctx, HU_VREC_LABEL_SEND, 3000))
        goto restore;
    if (p->press(p->ctx, HU_VREC_LABEL_SEND) != HU_OK)
        goto restore;
    sent = true;
    out->stage = HU_VREC_STAGE_SENT;
    rc = HU_OK;

restore:
    if (!sent && out->stage >= HU_VREC_STAGE_RECORD)
        (void)p->press(p->ctx, HU_VREC_LABEL_CANCEL);
    p->playback_dispose(p->ctx);
    (void)p->set_input(p->ctx, req->real_mic);
    out->restored = input_is(p, req->real_mic);
    p->restore_ui(p->ctx);
    if (sent)
        out->verified = p->audio_row_after(p->ctx, req->handle, req->handle_len,
                                           out->prior_max_rowid, 10000);
    return rc;
}
```

- [ ] **Step 4:** Build + `--filter=vrec` — Expected: all 14 PASS. (`hu_voice_record_send` has no product caller until Task 4; it lives in the already-loaded object from Task 1 and is referenced by tests, so neither dead-strip counter moves.)
- [ ] **Step 5: Mutation checks** (each must make ≥1 named test FAIL, then restore): (a) delete the `press(CANCEL)` line → playback/missing-send tests fail; (b) replace `input_is(p, HU_VREC_BLACKHOLE_NAME)` with `true` → readback test fails.
- [ ] **Step 6: Commit** `feat(voice): orchestrate Messages voice recording with guaranteed restore`.

---

### Task 3: macOS port + chat.db helpers

**Files:**
- Create: `src/channels/imessage_voice_record_macos.c` (whole body `#if defined(__APPLE__) && defined(__MACH__) && !HU_IS_TEST`; `#else` provides `hu_voice_record_macos_port()` returning a port whose `gather_facts` returns `HU_ERR_NOT_SUPPORTED` so the orchestrator always blocks, and whose every other member is a safe no-op stub (`HU_ERR_NOT_SUPPORTED` / `false` / `-1` / nothing) so no member is NULL — keeps the symbol linkable on every variant)
- Modify: `src/channels/imessage.c` (add two exported helpers next to the existing `SELECT MAX(ROWID)` at ~line 4655, reusing its db-open code; and upgrade Task 1's log-only block: on `HU_VREC_ROUTE_SHADOW` call `hu_voice_record_macos_port()->gather_facts(...)` with `getenv("HU_VOICE_REAL_INPUT")`, set `min_idle_sec` from `HU_VOICE_MIN_IDLE_SEC` (default 20), and log `voice delivery shadow: would_record=<0|1> block=<name>` — this is the port's product caller; the attachment send still happens), `include/human/channels/imessage_voice_record.h` (declare them + the port factory), `CMakeLists.txt` (add the new source to the unconditional core list next to Task 1's; add `"-framework CoreAudio" "-framework AudioToolbox"` to the Apple `target_link_libraries(human_core …)` at ~line 2096)
- Test: `tests/test_imessage_voice_record.c` (one test: the non-live port always blocks)

**Interfaces — Produces:**

```c
const hu_voice_record_port_t *hu_voice_record_macos_port(void);
int64_t hu_imessage_chatdb_max_rowid(void);                      /* -1 on error */
bool hu_imessage_chatdb_audio_from_me_after(const char *handle, size_t handle_len,
                                            int64_t after_rowid); /* one query, no wait */
```

`audio_row_after` in the port polls `hu_imessage_chatdb_audio_from_me_after` every 250 ms until `timeout_ms`. The chat.db query (inside `imessage.c`, `SQLITE_STATIC`, path via `hu_paths_chatdb`):

```sql
SELECT 1 FROM message m
JOIN chat_message_join cj ON cj.message_id = m.ROWID
JOIN chat c ON c.ROWID = cj.chat_id
WHERE m.ROWID > ?1 AND m.is_from_me = 1 AND m.is_audio_message = 1
  AND c.chat_identifier = ?2
LIMIT 1
```

**Port implementation requirements** (each maps to one static function; follow the named existing code for idiom):

| Port fn | Implementation |
|---|---|
| `gather_facts` | `AXIsProcessTrusted()`; Messages pid via `proc_listallpids` + `proc_name == "Messages"` (copy the idiom of `ax_messages_pid`, `imessage.c:3795`); CoreAudio device list (`kAudioHardwarePropertyDevices`) → match `kAudioObjectPropertyName` against `HU_VREC_BLACKHOLE_NAME` and `real_mic`, input-capable = `kAudioDevicePropertyStreams` scope input size > 0; `real_mic_busy` = `kAudioDevicePropertyDeviceIsRunningSomewhere` on the real mic; idle = IOKit `IOHIDSystem` `HIDIdleTime` (ns → s) |
| `set_input` / `get_input` | `kAudioHardwarePropertyDefaultInputDevice` on `kAudioObjectSystemObject`; `get_input` returns the device's `kAudioObjectPropertyName` |
| `remember_ui` / `restore_ui` | remember `NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier` and the Messages main window's `AXTitle`; restore: if the title's conversation is a button in the window (AX `AXButton` with that title), press it; then re-activate the remembered pid with `runningApplicationWithProcessIdentifier:` + `activateWithOptions:` (idiom of `ax_activate_messages`, `imessage.c:3883`). Best-effort; log if not restored. |
| `open_chat` | `NSWorkspace openURL:` with `imessage://<handle>` via objc runtime (no fork/exec), then 300 ms settle |
| `press` / `wait_label` | depth-first walk (max depth 40) of the Messages focused window for `AXButton` whose `AXDescription` or `AXTitle` equals the label; `AXUIElementPerformAction(kAXPressAction)`; `wait_label` polls every 50 ms |
| `playback_prepare` / `playback_run` / `playback_dispose` | `AudioFileOpenURL` → data format + magic cookie → `AudioQueueNewOutput` (internal thread) → `kAudioQueueProperty_CurrentDevice` = BlackHole UID (`kAudioDevicePropertyDeviceUID`) → 3 buffers pre-filled → `AudioQueuePrime`. `run`: `AudioQueueStart`, refill in callback until EOF, `AudioQueueStop(q, false)`, poll `kAudioQueueProperty_IsRunning` every 10 ms up to duration + 2 s |
| `sleep_ms` | `usleep(ms * 1000)` |

- [ ] **Step 1: Failing test** (all builds; the test binary is `HU_IS_TEST`, so it exercises the stub):

```c
static void test_vrec_macos_port_blocks_under_test(void) {
    const hu_voice_record_port_t *p = hu_voice_record_macos_port();
    HU_ASSERT_NOT_NULL(p);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(p, &r, &res), HU_ERR_NOT_SUPPORTED);
}
```

- [ ] **Step 2:** Build — Expected: undeclared `hu_voice_record_macos_port`.
- [ ] **Step 3:** Implement per the table; build `human` (non-test, Apple branch compiles) and `human_tests` (stub branch). Expected: both link; `grep -c '#include <sqlite3.h>' src/channels/imessage_voice_record_macos.c` → 0.
- [ ] **Step 4:** `--filter=vrec` PASS; `bash scripts/check-sqlite-includer-ratchet.sh` and `bash scripts/check-test-source-gate-symmetry.sh` pass.
- [ ] **Step 5: Commit** `feat(voice): macOS port for Messages voice recording (CoreAudio, AudioQueue, AX)`.

---

### Task 4: Route memo sends through the orchestrator + operator CLI

**Files:**
- Modify: `src/channels/imessage.c` `imessage_send` (production branch, before the `imsg_media:` attachment loop at ~line 2518), `src/app/main.c` (`cmd_voice`: add `record-send` next to `preview` at ~line 2819)
- Test: `tests/test_imessage_voice_record.c`

**Interfaces — Consumes:** `hu_voice_record_route` (Task 1), `hu_voice_record_send` (Task 2), `hu_voice_record_macos_port` (Task 3). **Produces:** the live RECORD branch and the `record-send` CLI.

Behavior in `imessage_send` (production only; `HU_IS_TEST` path unchanged), replacing the `(not yet active)` log for RECORD:
- `RECORD`: build a request (`real_mic = getenv("HU_VOICE_REAL_INPUT")`, `min_idle_sec` from `HU_VOICE_MIN_IDLE_SEC` default 20, `seed = time ^ pid`) and call `hu_voice_record_send(hu_voice_record_macos_port(), …)`.
  - `HU_OK` ⇒ `imessage_report_sent(tgt, tgt_len, NULL, 0, HU_IMESSAGE_SENT_KIND_MEDIA, res.prior_max_rowid)`; log `verified`/`restored`; if `!restored` log at error level; **return HU_OK without the attachment send**.
  - `HU_ERR_NOT_SUPPORTED` / `HU_ERR_IO` ⇒ log block/stage; fall through to the attachment send.
- Gate comment at this site: `/* HU_VOICE_DELIVERY=messages as a default is gated on the W5 "real or clone?" measurement (spec 2026-09-26 W5): do not flip without it. */`

CLI (`human voice record-send --to <handle> --file <path>`): runs exactly the RECORD branch above (same env) and prints `stage=… block=… verified=… restored=…`; exit 0 only when `verified && restored`. For operator testing against the self-chat; not used by the daemon.

This task's behavior is only reachable in the production (non-`HU_IS_TEST`) build, where the orchestrator's contract is already pinned by Task 2's fake-port tests; its own verification is Task 5's live run. TDD here means: the RECORD branch's decision logic stays in the tested pure functions, and the branch itself only translates `hu_voice_record_send`'s three return classes (per the bullets above).

- [ ] **Step 1:** Implement the RECORD branch (with the gate comment) and the `record-send` CLI.
- [ ] **Step 2:** Confirm the three return classes map as specified by reading the branch against the bullets, and that `HU_ERR_NOT_SUPPORTED`/`HU_ERR_IO` both reach the unchanged attachment loop (no early return).
- [ ] **Step 3:** Full build (`human`, `human_tests`), full suite — Expected: `Results: N/N passed`, 0 ASan. Wiring check: `grep -n 'hu_voice_record_send\|hu_voice_record_route' src/channels/imessage.c src/app/main.c` shows callers outside `imessage_voice_record.c`.
- [ ] **Step 4: Commit** `feat(voice): deliver iMessage voice replies through Messages when HU_VOICE_DELIVERY=messages`.

---

### Task 5: Live verification on the self-chat (manual, evidence-producing)

No code unless a defect is found (then TDD it into Tasks 1–4's tests). Preconditions: BlackHole installed, Seth at the Mac, sends only to the self-chat `+18012017497`.

- [ ] **Step 1:** Generate a clip: `./build/human voice preview --persona seth --text "<short casual line>" --out /private/tmp/claude-501/w3/clip.caf`.
- [ ] **Step 2:** Guard checks, each must print a block and send nothing: (a) `HU_VOICE_REAL_INPUT` unset → `block=no_real_mic`; (b) run with `HU_VOICE_MIN_IDLE_SEC=100000` → `block=user_active`.
- [ ] **Step 3:** Live run: `HU_VOICE_DELIVERY=messages HU_VOICE_REAL_INPUT="Shure MV7" HU_VOICE_MIN_IDLE_SEC=0 ./build/human voice record-send --to +18012017497 --file /private/tmp/claude-501/w3/clip.caf` — Expected: `verified=1 restored=1`, exit 0.
- [ ] **Step 4: Measure the artifact**, not the log: newest self-chat row has `is_from_me=1, is_audio_message=1`; `SwitchAudioSource -c -t input` = `Shure MV7`; `silencedetect` leading silence ≤ 0.9 s (spike: 2.9 s); `volumedetect` max_volume vs the source clip — record the dB delta as the measured level offset. If the offset exceeds 3 dB, file it as a finding for the persona's Cartesia `volume` (do not add DSP gain).
- [ ] **Step 5:** Seth listens on his phone and compares with a memo he records himself; note his verdict in the ledger.
