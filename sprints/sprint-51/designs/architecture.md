---
title: "Sprint 51 — Architectural design (cross-cutting)"
created: 2026-05-24
sprint: 51
status: design-only
---

# Sprint 51 cross-cutting design

Per-story acceptance criteria live in `../stories.md`. This file
captures the architectural decisions every story leans on.

## 1. Step vtable

```c
/* include/human/onboard/step.h */

typedef struct hu_onboard_state hu_onboard_state_t;

typedef enum hu_onboard_step_result {
    HU_ONBOARD_NEXT = 0,        /* advance to next_default */
    HU_ONBOARD_BACK = 1,        /* pop step history */
    HU_ONBOARD_REPEAT = 2,      /* re-render current step (e.g. validation failed) */
    HU_ONBOARD_QUIT = 3,        /* save state, exit cleanly (resume-able) */
    HU_ONBOARD_COMPLETE = 4,    /* terminal — wizard finished */
    HU_ONBOARD_ABORT = 5,       /* unrecoverable — print docs link, exit nonzero */
} hu_onboard_step_result_t;

typedef struct hu_onboard_step {
    const char *name;                  /* kebab-case, stable */
    const char *display_name;          /* "Provider setup" */
    /* Render prompt + read user input + apply to state. Returns one of
     * the result codes above. */
    hu_onboard_step_result_t (*run)(struct hu_onboard_step *self,
                                    hu_onboard_state_t *state);
    /* Optional pre-validation: called before run() to render any saved
     * answers (resume path) or pre-flight checks. */
    void (*enter)(struct hu_onboard_step *self,
                  hu_onboard_state_t *state);
    void *user_data;
} hu_onboard_step_t;
```

### Step rules

- `run` MUST persist its result into `state` BEFORE returning NEXT
  (so a crash post-step preserves the answer). Tested by injecting a
  fault between run() returning and dispatcher transitioning.
- A `run` that does NETWORK I/O MUST gate on `HU_IS_TEST` — same
  discipline as Sprint 50 C3.3. Enforced by `scripts/check-onboard-
  test-guards.sh` pre-commit.
- Each step is independently testable — instantiate the step, build a
  minimal state, call `run`, inspect the state mutation.

## 2. State persistence

```c
typedef enum hu_onboard_step_id {
    HU_ONBOARD_STEP_WELCOME = 0,
    HU_ONBOARD_STEP_PROVIDER,
    HU_ONBOARD_STEP_PERSONA,
    HU_ONBOARD_STEP_CHANNELS,
    HU_ONBOARD_STEP_TESTSEND,
    HU_ONBOARD_STEP_COMPLETE,
} hu_onboard_step_id_t;

typedef struct hu_onboard_state {
    int schema_version;          /* 1 — pinned */
    hu_onboard_step_id_t current;
    hu_onboard_step_id_t history[10];
    size_t history_depth;
    /* Per-step persisted answers — union or tagged variant. */
    struct {
        char provider_name[32];
        bool provider_smoke_passed;
    } provider;
    struct {
        char template_choice;    /* '1' | '2' | '3' | 'm' for markdown-import */
        char markdown_path[512]; /* iff template_choice=='m' */
    } persona;
    struct {
        bool imessage_enabled;
        bool slack_enabled;
        bool discord_enabled;
        bool telegram_enabled;
        bool imessage_fda_pending;
    } channels;
    struct {
        char contact_handle[128];
        bool test_send_succeeded;
    } testsend;
} hu_onboard_state_t;
```

### Save semantics

- `hu_onboard_state_save(state, path)` writes `tmp + fwrite + fflush +
  fsync + rename(tmp, path)` — same atomic pattern as
  `hu_personal_model_save` (pinned by
  `tests/test_personal_model_atomic_save.c`).
- Schema version 1 is locked at sprint close. Future additions ship as
  schema v2 with an explicit migration path (out of scope here).

## 3. Dispatcher

```c
hu_error_t hu_onboard_run(hu_allocator_t *alloc,
                          hu_onboard_state_t *state,
                          hu_onboard_step_t *step_table[],
                          size_t step_count);
```

Single loop:

```
while (state->current != HU_ONBOARD_STEP_COMPLETE) {
    step = step_table[state->current];
    step->enter(step, state);
    result = step->run(step, state);
    hu_onboard_state_save(state, "~/.human/onboard-state.json");
    switch (result) {
      case NEXT:     push history; state->current = next_default(current); break;
      case BACK:     state->current = pop_history();                       break;
      case REPEAT:   /* don't change current */                             break;
      case QUIT:     return HU_OK;
      case COMPLETE: state->current = HU_ONBOARD_STEP_COMPLETE;            break;
      case ABORT:    return HU_ERR_FAILED;
    }
}
```

## 4. Copy file convention

User-facing strings >2 lines live in `docs/copy/onboarding-step<N>.md`
rather than inline C string literals. Rationale:

- Wording fixes don't require recompilation
- Test that captures the rendered output also captures the source
  file's last-known-good content — so a wording PR forces a test
  update, which forces the author to re-read the change.
- The copy file is part of the sprint's deliverable surface and gets
  reviewed at sprint close.

## 5. TTY check for auto-trigger (US-C2.8)

```c
static bool is_interactive(void) {
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}
```

Gate the first-run auto-trigger on this. Non-interactive runs print
to stderr:

```
human: first-run setup required.
       Run `human onboard` interactively. See https://human.example/getting-started
```

…and exit 1. Tested with `setsid` to detach from any TTY in the test.

## 6. Build wire-up pattern

Same shape as Sprint 50's CMake convention:

```cmake
list(APPEND HU_CORE_SOURCES
    src/onboard/state.c
    src/onboard/dispatcher.c
    src/onboard/step_welcome.c
    src/onboard/step_provider.c
    src/onboard/step_persona.c
    src/onboard/step_channels.c
    src/onboard/step_testsend.c
    src/onboard/persona_templates.c
)

list(APPEND HU_TEST_SOURCES
    tests/test_onboard_state.c
    tests/test_onboard_step1.c
    tests/test_onboard_step_provider.c
    tests/test_onboard_step_persona.c
    tests/test_onboard_step_channels.c
    tests/test_onboard_step_testsend.c
    tests/test_onboard_resume.c
    tests/test_onboard_e2e_resume.c
    tests/test_first_run_trigger.c
)
```

Per `.claude/rules/test-source-gate-symmetry.md`: every source +
test pair lands atomically. The pre-commit hook fires if they drift.

## 7. Test fixtures

- `tests/fixtures/onboard_state_v1.json` — known-good state file for
  schema-validation tests
- `tests/fixtures/onboard_persona_import.md` — sample markdown-import
  input for US-C2.4
- `tests/fixtures/onboard_persona_import_malformed.md` — adversarial
  input that fails validation
- The mock channel from `tests/test_imessage_ingest.c` is reused for
  US-C2.6 test-send

## 8. Sizing recheck

| Story    | Source LoC | Test LoC | Total |
|----------|-----------|----------|-------|
| US-C2.1  | ~430      | ~200     | 630   |
| US-C2.2  | ~80       | ~120     | 200   |
| US-C2.3  | ~300      | ~400     | 700   |
| US-C2.4  | ~450      | ~350     | 800   |
| US-C2.5  | ~200      | ~250     | 450   |
| US-C2.6  | ~280      | ~350     | 630   |
| US-C2.7  | ~50       | ~250     | 300   |
| US-C2.8  | ~80       | ~200     | 280   |
| **Total**| **~1870** | **~2120**| **~3990** |

Source LoC tracks backlog's "~2000 LoC" estimate; test density ~1.1× source.

## 9. Where this design intentionally STOPS

- No abstract dialog/widget framework — each step is hand-rolled
  prompt + read loop. A framework would require a separate sprint
  worth of design and offer no real value for 5 steps.
- No animation / spinners beyond the existing `hu_term_spinner`
  helper. CLI vibe, not GUI vibe.
- No multi-language UX. English-only at ship time.
- No retry-with-different-key for cloud providers (Step 2 just loops
  back to the choice screen if the smoke fails — user pastes the
  corrected key on the retry).
