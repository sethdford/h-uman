# Phase 2 — Split the 14,750-LOC `daemon.c` God-File

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Decompose `src/daemon.c` (14,750 LOC, 6 responsibilities) into single-responsibility translation units under `src/daemon/`, each independently testable. **Behavior must not change** — this is a pure structural move verified by the full suite.

**Architecture:** Extract the 4 *cohesive* buckets first (director, identity_state, peripheral_gov, message_router — ~840 LOC of clearly-bucketed functions). Their single-bucket file-scope statics move with them; the 3 genuinely cross-bucket statics go into a `daemon_common.h` shared-state header. The `service_lifecycle` giant (`hu_service_run` 3,759 LOC + `hu_service_run_proactive_checkins` 1,496 LOC) is explicitly **Phase 2b** — splitting a 3,759-line function is different, riskier surgery and is scoped separately so this phase stays safe and shippable.

**Tech Stack:** C11, CMake static-lib source lists, `HU_TEST_*` macros, ASan dev build.

**Hard constraint (the real difficulty, per inventory):** these 3 statics are shared across 2 buckets and are process-lifetime — they must remain reachable by their callers after extraction:
| Static | Touched by | Move target |
|---|---|---|
| `g_identity_graph` / `_loaded` | message_router, identity_state | `daemon_common.h` (extern) |
| `gov_budget` / `_inited` | peripheral_gov, service_lifecycle | `daemon_common.h` (extern) |
| `g_proactive_throttle` / `_initialized` | peripheral_gov, service_lifecycle | stays; accessor `daemon_throttle()` exported |

---

## File Structure

| New file | Functions (from inventory) | Owns statics |
|---|---|---|
| `src/daemon/daemon_director.c` + `include/human/daemon/director.h` | `hu_daemon_detect_emotion`, `hu_daemon_parse_director_result`, `hu_daemon_director_call`, `classify_comfort_response_type`, `record_topic_baselines_from_text`, `score_comfort_engagement`, `store_conversation_summary` (~484 LOC) | `g_classify_provider*` (4) |
| `src/daemon/daemon_identity.c` + `include/human/daemon/identity_state.h` | `trust_find_or_create_slot`, `hu_daemon_get_trust_state`, `hu_daemon_set_trust_state`, `hu_daemon_trust_count`, `hu_daemon_trust_reset` (~71 LOC) | `g_contact_trust[4096]`, `g_contact_trust_count`, `g_trust_mutex` |
| `src/daemon/daemon_peripheral_gov.c` + `include/human/daemon/peripheral_gov.h` | `hu_daemon_visual_attach_gov_allow`, `_after_send`, `daemon_throttle` (~31 LOC) | `hu_daemon_visual_attach_gov*` (2), `g_proactive_throttle*` |
| `src/daemon/daemon_message_router.c` + `include/human/daemon/message_router.h` | `cross_channel_format_when`, `cross_channel_platform_label`, `daemon_cross_ctx_append_line`, `hu_daemon_dispatch_imessage_reply` (~253 LOC) | (uses `g_identity_graph` via common) |
| `include/human/daemon/common.h` | — | externs: `g_identity_graph`, `gov_budget` + their flags |

Remaining in `daemon.c` after this phase: `service_lifecycle` (~5,465 LOC) + `shared_util` (~92 LOC). Down from 14,750 to ~13,900 across the largest TU, but now 4 testable modules exist and the god-file has a clear "only the service loop lives here" identity. **Phase 2b** target: `service_lifecycle`.

---

### Task 1: Establish the shared-state header (unblocks everything)

**Files:**
- Create: `include/human/daemon/common.h`
- Modify: `src/daemon.c` (change the 3 cross-bucket statics from `static` to non-static + add the externs via the header)

- [ ] **Step 1: Write the shared-state header**

```c
/* include/human/daemon/common.h */
#ifndef HU_DAEMON_COMMON_H
#define HU_DAEMON_COMMON_H

#include "human/identity_graph.h"
#include "human/proactive_budget.h" /* hu_proactive_budget_t */
#include <stdbool.h>

/* Cross-bucket daemon state. These are process-lifetime singletons shared by
 * more than one daemon module after the Phase 2 split. Defined in daemon.c
 * (until Phase 2b gives them a proper owner); declared here so the extracted
 * modules can reach them without re-introducing file-scope statics. */
extern hu_identity_graph_t g_identity_graph;
extern bool g_identity_graph_loaded;
extern hu_proactive_budget_t gov_budget;
extern bool gov_budget_inited;

#endif /* HU_DAEMON_COMMON_H */
```

- [ ] **Step 2: De-static the four definitions in `daemon.c`**

In `src/daemon.c`, change (around lines 183-184 and 525-532):

```c
/* was: static hu_identity_graph_t g_identity_graph; */
hu_identity_graph_t g_identity_graph;
/* was: static bool g_identity_graph_loaded; */
bool g_identity_graph_loaded;
/* was: static hu_proactive_budget_t gov_budget; */
hu_proactive_budget_t gov_budget;
/* was: static bool gov_budget_inited; */
bool gov_budget_inited;
```

Add near the top of `daemon.c`: `#include "human/daemon/common.h"`

- [ ] **Step 3: Build production binary (touch first) + full suite**

Run: `touch src/daemon.c && cmake --build build --target human -j8 && cmake --build build --target human_tests -j8 && ./build/human_tests`
Expected: `Linking C executable human` appears; 0 failures. No behavior change (same storage, just externally visible).

- [ ] **Step 4: Commit**

```bash
git add include/human/daemon/common.h src/daemon.c
git commit -m "refactor(daemon): expose cross-bucket state via daemon/common.h"
```

---

### Task 2: Extract the director bucket (with a characterization test)

**Files:**
- Create: `include/human/daemon/director.h`, `src/daemon/daemon_director.c`
- Create: `tests/test_daemon_director.c`
- Modify: `src/daemon.c` (remove the 7 functions + `g_classify_provider*`), `src/CMakeLists.txt`, `tests/test_main.c`

- [ ] **Step 1: Write a characterization test FIRST (lock current behavior of a pure-ish fn)**

`classify_comfort_response_type` is the cleanest pure function to pin. Promote it to a public symbol so it's testable, then characterize it.

```c
/* tests/test_daemon_director.c */
#include "human/daemon/director.h"
#include "test_harness.h"
#include <string.h>

static void director_classifies_greeting(void) {
    /* Characterization: pin the CURRENT output for known inputs. If you don't
     * know the expected enum yet, run once, observe, then assert that value —
     * the point is to detect CHANGE during extraction, not to redesign. */
    int t = hu_daemon_classify_comfort_response_type("hey, how are you?", 17);
    HU_ASSERT_EQ(t, /* observed value, e.g. */ 3 /* GREETING */);
}
static void director_classifies_comfort(void) {
    int t = hu_daemon_classify_comfort_response_type("I'm so sorry you're going through that", 38);
    HU_ASSERT_EQ(t, /* observed */ 1 /* COMFORT */);
}

void run_daemon_director_tests(void) {
    HU_TEST_SUITE("daemon_director");
    HU_RUN_TEST(director_classifies_greeting);
    HU_RUN_TEST(director_classifies_comfort);
}
```

- [ ] **Step 2: Run to observe + set the expected values**

Build once with the symbol exposed (Step 3), run `./build/human_tests --filter=daemon_director`, read the actual returned ints, and write them into the asserts. This is legitimate characterization (NOT pinning a bug — we're freezing known-good behavior across a move).

- [ ] **Step 3: Create the header**

```c
/* include/human/daemon/director.h */
#ifndef HU_DAEMON_DIRECTOR_H
#define HU_DAEMON_DIRECTOR_H
#include "human/agent.h"
#include "human/provider.h"
#include <stddef.h>

/* Director / response-classification bucket extracted from daemon.c.
 * Owns g_classify_provider (the Flash-Lite meta-behavior classifier). */
int hu_daemon_classify_comfort_response_type(const char *text, size_t len);
/* ... declare the other 6 functions with their existing signatures, copied
 *     verbatim from daemon.c (detect_emotion, parse_director_result,
 *     director_call, record_topic_baselines_from_text, score_comfort_engagement,
 *     store_conversation_summary). Make formerly-static ones public via hu_daemon_*. */
#endif
```

- [ ] **Step 4: Move the 7 functions + 4 statics into `daemon_director.c`**

Cut the function bodies (inventory lines 221-329, 661-928 region) and the `g_classify_provider*` statics (lines 168-171) from `daemon.c` into `src/daemon/daemon_director.c`. Add `#include "human/daemon/director.h"` to both files. The statics stay `static` in the new file (single-bucket).

- [ ] **Step 5: Register in CMake + runner**

In `src/CMakeLists.txt` add `src/daemon/daemon_director.c` to core sources and `tests/test_daemon_director.c` to test sources. In `tests/test_main.c` declare+call `run_daemon_director_tests`.

- [ ] **Step 6: Build production binary + FULL suite**

Run: `touch src/daemon.c src/daemon/daemon_director.c && cmake --build build --target human -j8 && cmake --build build --target human_tests -j8 && ./build/human_tests`
Expected: `Linking C executable human`; 0 failures, 0 ASan errors; the 2 characterization tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/human/daemon/director.h src/daemon/daemon_director.c \
        tests/test_daemon_director.c src/CMakeLists.txt tests/test_main.c src/daemon.c
git commit -m "refactor(daemon): extract director/classification bucket to daemon/director.c"
```

---

### Task 3: Extract identity_state bucket

**Files:** Create `include/human/daemon/identity_state.h`, `src/daemon/daemon_identity.c`; modify `daemon.c`, `src/CMakeLists.txt`. (Test runners for trust state already exist — `hu_daemon_trust_count`/`_reset` are test hooks; reuse them.)

- [ ] **Step 1: Header** — declare `hu_daemon_get_trust_state`, `hu_daemon_set_trust_state`, `hu_daemon_trust_count`, `hu_daemon_trust_reset` (signatures verbatim from `daemon.c:1125-1161`).
- [ ] **Step 2: Move** the 5 functions (inventory lines 928-1161) + statics `g_contact_trust[4096]`, `g_contact_trust_count`, `g_trust_mutex` (lines 916-920) into `daemon_identity.c`. Keep statics `static` (single-bucket). Add `#include "human/daemon/common.h"` if `trust_find_or_create_slot` reads `g_identity_graph`.
- [ ] **Step 3: CMake** — add the source.
- [ ] **Step 4: Build prod + full suite** — `touch src/daemon.c src/daemon/daemon_identity.c && cmake --build build --target human -j8 && cmake --build build --target human_tests -j8 && ./build/human_tests`. Expected: 0 failures; existing trust tests still green (they call the now-relocated `hu_daemon_*` symbols).
- [ ] **Step 5: Commit** — `refactor(daemon): extract identity/trust state to daemon/identity.c`

---

### Task 4: Extract peripheral_gov bucket

**Files:** Create `include/human/daemon/peripheral_gov.h`, `src/daemon/daemon_peripheral_gov.c`; modify `daemon.c`, `src/CMakeLists.txt`.

- [ ] **Step 1: Header** — declare `hu_daemon_visual_attach_gov_allow`, `_after_send`, and `daemon_throttle` (export `daemon_throttle` as `hu_daemon_throttle` so service_lifecycle can still reach `g_proactive_throttle` via the accessor — this is the "keep the static, export the accessor" resolution from the blocker table).
- [ ] **Step 2: Move** the 3 functions (lines 538-553, 1207-1213) + statics `hu_daemon_visual_attach_gov*` (535-536) and `g_proactive_throttle*` (1204-1205). `gov_budget` stays in `daemon.c` but is now reachable via `common.h` (Task 1).
- [ ] **Step 3: CMake + build prod + full suite + commit** — same pattern; message `refactor(daemon): extract peripheral governance to daemon/peripheral_gov.c`.

---

### Task 5: Extract message_router bucket

**Files:** Create `include/human/daemon/message_router.h`, `src/daemon/daemon_message_router.c`; modify `daemon.c`, `src/CMakeLists.txt`.

- [ ] **Step 1: Header** — declare `hu_daemon_dispatch_imessage_reply` (verbatim sig from `daemon.c:976`) and the 3 cross-channel helpers (promote to `hu_daemon_*`).
- [ ] **Step 2: Move** the 4 functions (lines 560-660, 976-1124). They read `g_identity_graph` via `common.h` (Task 1). The `k_daemon_configs` table (line 469) is read by `get_active_daemon_config` (shared_util, stays in daemon.c) AND the router — keep `k_daemon_configs` in `daemon.c` and export `get_active_daemon_config` as `hu_daemon_get_active_config` so the router calls the accessor rather than the table directly.
- [ ] **Step 3: CMake + build prod + full suite + commit** — `refactor(daemon): extract message router to daemon/message_router.c`.

---

### Task 6: Verify the split end-to-end against the running daemon

- [ ] **Step 1: Full suite + ASan**

Run: `cmake --build build --target human_tests -j8 && ./build/human_tests`
Expected: 0 failures, 0 ASan errors. Count should match the pre-Phase-2 total exactly (plus the 2 new director characterization tests).

- [ ] **Step 2: Confirm daemon.c shrank and modules exist**

Run: `wc -l src/daemon.c src/daemon/*.c`
Expected: `daemon.c` ≈ 13,900; four new files totalling ≈ 840 LOC.

- [ ] **Step 3: Behavioral smoke test (production binary, per quality-gates)**

Run the daemon against a fixture inbound and confirm a reply still routes (use the existing daemon smoke harness / `scripts/agent-preflight.sh`). Evidence, not assertion (`.claude/rules/quality-gates.md`).

---

## Phase 2b (follow-on, NOT this plan)

`hu_service_run` (3,759 LOC) + `hu_service_run_proactive_checkins` (1,496 LOC) are
the remaining giants. Splitting them means decomposing a single 3,759-line function
into the inbound-loop / outbound-loop / tick-scheduler / worker-pool-lifecycle
sub-functions, with `gov_budget` mutation crossing the peripheral_gov boundary
(inverts dependency — resolve with an explicit `hu_daemon_gov_*` mutator API rather
than direct field writes). Track as a separate plan; it needs its own
characterization-test scaffold before any cut.

## Self-Review

- **Spec coverage:** 4 cohesive buckets extracted (Tasks 2-5), shared state handled (Task 1), service_lifecycle explicitly deferred (Phase 2b). ✓
- **No placeholders:** function lists are exact (from inventory line numbers); the one genuine unknown (characterization expected enum values) has an explicit "observe-then-assert" step, not a TODO. ✓
- **Type consistency:** every moved function keeps its verbatim signature; promoted statics become `hu_daemon_*` and are declared once per header, called by name. ✓
- **Behavior preservation:** every task ends in the FULL suite + production-binary rebuild (touch-first per stale-binary rule). No logic edits, only moves. ✓
