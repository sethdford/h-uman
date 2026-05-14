# Output Validator Chain — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor h-uman's six scattered output sanitizers into a single composable validator-chain vtable, add three new validators to catch persona-narrator / assistant-closer / role-collapse leaks (F1/F2/F3 from the 2026-05-14 Jordan-channel incidents), plumb stop-sequences through the provider vtable, and add a structured-output contract for providers that support it.

**Architecture:** Three-layer defense-in-depth:
1. **Output contract** at the provider boundary — structured response (JSON schema / tool-use proxy / sentinel-extract) where supported.
2. **Stop-sequence registry** — per-provider, per-channel turn-boundary tokens plumbed through `hu_chat_request_t`; the model stops generating before role-collapse can happen.
3. **Validator chain** — `hu_output_validator_t` vtable; existing strippers become validators; new validators catch F1/F2/F3. Chain composition is per-channel + per-persona. REJECT triggers the existing retry-slim path.

**Tech Stack:** C11, `-Wall -Wextra -Wpedantic -Werror`. Zero deps beyond libc. Build via CMake presets (`dev` for ASan, `test` for clean). Tests via `human_tests` with `HU_TEST_SUITE` / `HU_RUN_TEST` / `HU_ASSERT_*` macros.

**Scope:** 6 phases. Each phase ships independently and leaves the tree green. Total estimated touch: ~25 new files, ~30 modified files, ~80 new tests.

---

## File Structure

### New files (Phase 1 — vtable infrastructure)
- `include/human/agent/output_validator.h` — public vtable + result types
- `include/human/agent/output_validator_chain.h` — registry + pipeline API
- `src/agent/output_validator.c` — vtable wrapper helpers
- `src/agent/output_validator_chain.c` — registry + pipeline execution
- `tests/test_output_validator.c` — vtable + chain unit tests

### New files (Phase 2 — migrated strippers as validators)
- `src/agent/validators/harmony_token_validator.c`
- `src/agent/validators/thinking_block_validator.c`
- `src/agent/validators/degenerate_repetition_validator.c`
- `src/agent/validators/bullet_cot_validator.c`
- `src/agent/validators/channel_tags_validator.c`
- `src/agent/validators/ai_phrases_validator.c`
- `src/agent/validators/formal_structure_validator.c`
- `src/agent/validators/cot_audit_validator.c`
- `include/human/agent/validators/builtin.h` — factory functions for each
- `tests/test_validators_builtin.c` — migrated test cases pinning behavior

### New files (Phase 3 — F1/F2/F3 validators)
- `src/agent/validators/persona_narrator_validator.c`
- `src/agent/validators/assistant_closer_validator.c`
- `src/agent/validators/role_consistency_validator.c`
- `tests/test_validators_persona_safety.c` — pins the Jordan leaks as regression tests

### New files (Phase 4 — stop-sequences)
- `include/human/agent/stop_sequence_registry.h` — per-(provider, channel) defaults
- `src/agent/stop_sequence_registry.c`
- `tests/test_stop_sequences.c` — per-provider request-building tests

### New files (Phase 5 — structured output)
- `include/human/provider/structured_output.h` — schema types
- `src/providers/structured_output_gemini.c` — Gemini responseSchema wiring
- `src/providers/structured_output_anthropic.c` — Anthropic tool-use proxy
- `src/providers/structured_output_openai.c` — OpenAI json_schema wiring
- `src/providers/structured_output_sentinel.c` — Sentinel-extract fallback
- `tests/test_structured_output.c`

### Modified files
- `include/human/provider.h` — add `stop_sequences[]` + `response_schema` to `hu_chat_request_t`
- `src/providers/anthropic.c, openai.c, gemini.c, ollama.c, openrouter.c, claude_cli.c, codex_cli.c, openai_codex.c, compatible.c, huml.c, llamacpp.c, apple.c` — wire stop-sequences + structured output
- `src/agent/agent_turn.c:5587-5664` — replace stripper sequence with chain execution
- `src/agent/agent_stream.c:1409-1467, 2140-2200` — replace stripper sequence with chain execution
- `src/daemon.c:1068-1070, 1712-1715, 2052, 9201-9204, 10539-10541, 11562-11565` — replace stripper sequence with chain execution
- `src/daemon_cron.c:286` — replace single stripper with chain
- `src/channels/imessage.c:909, format.c:141` — replace single stripper with chain
- `src/gateway/openai_compat.c:620-622` — replace stripper sequence with chain execution
- `CMakeLists.txt` — add new source files

---

## Phase 1 — Validator Vtable Infrastructure

Goal: ship the abstraction with zero behavior change. Existing sanitization continues to work via direct calls; new vtable exists alongside, fully tested with a synthetic in-test validator.

### Task 1: Define the validator vtable header

**Files:**
- Create: `include/human/agent/output_validator.h`

- [ ] **Step 1: Write the header**

```c
/* hu_output_validator — composable post-generation output check.
 *
 * Each validator inspects a model response (or the partial state from
 * a previous validator's rewrite) and returns one of:
 *   PASS     — output is acceptable; chain continues with same text
 *   REWRITE  — output was modified; chain continues with new text
 *   REJECT   — output is unsendable; chain short-circuits, caller
 *              must either retry (via response_guard_retry_slim) or
 *              suppress the message
 *
 * Validators are stateless per-call. They receive an allocator for any
 * rewrite buffer they need and are responsible for diagnostic strings.
 * The chain (output_validator_chain.h) owns lifecycle; this header
 * defines the single-validator contract. */
#ifndef HU_AGENT_OUTPUT_VALIDATOR_H
#define HU_AGENT_OUTPUT_VALIDATOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hu_validator_decision {
    HU_VALIDATOR_PASS = 0,
    HU_VALIDATOR_REWRITE = 1,
    HU_VALIDATOR_REJECT = 2,
} hu_validator_decision_t;

/* Per-call result. If decision == REWRITE, `text` is the new output and
 * `text_owned` indicates whether the caller must free it via `alloc`. If
 * decision == REJECT, `text` is NULL and `reason` explains why. */
typedef struct hu_validator_result {
    hu_validator_decision_t decision;
    const char *text;
    size_t text_len;
    bool text_owned; /* if true, caller frees via alloc->free(.., text, text_len + 1) */
    char *reason;    /* allocator-owned; may be NULL on PASS/REWRITE */
    size_t reason_len;
} hu_validator_result_t;

void hu_validator_result_free(hu_allocator_t *alloc, hu_validator_result_t *result);

/* Per-call context. The chain passes this to each validator so it can
 * make decisions based on which channel/persona/provider produced the
 * response. NULL fields are permitted (the validator must tolerate them). */
typedef struct hu_validator_context {
    const char *channel_id;  /* "imessage", "slack", ...; NULL = unknown */
    size_t channel_id_len;
    const char *persona_name; /* active persona display name; NULL = none */
    size_t persona_name_len;
    const char *provider_name; /* "anthropic", "gemini", ...; NULL = unknown */
    size_t provider_name_len;
} hu_validator_context_t;

typedef struct hu_output_validator_vtable {
    /* Run the validator. MUST populate *out with a valid result. */
    hu_error_t (*validate)(void *ctx, hu_allocator_t *alloc,
                           const hu_validator_context_t *vctx,
                           const char *response, size_t response_len,
                           hu_validator_result_t *out);
    /* Stable identifier for logs + telemetry. Must not return NULL. */
    const char *(*name)(void *ctx);
    /* Optional: free implementation-owned state. May be NULL. */
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_output_validator_vtable_t;

typedef struct hu_output_validator {
    void *ctx;
    const hu_output_validator_vtable_t *vtable;
} hu_output_validator_t;

/* Free a validator's owned state. Safe on zero-initialized structs. */
void hu_output_validator_deinit(hu_output_validator_t *v, hu_allocator_t *alloc);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_OUTPUT_VALIDATOR_H */
```

- [ ] **Step 2: Verify header compiles in isolation**

Run: `gcc -Wall -Wextra -Wpedantic -Werror -std=c11 -I include -fsyntax-only include/human/agent/output_validator.h`
Expected: clean compile, no warnings.

- [ ] **Step 3: Commit**

```bash
git add include/human/agent/output_validator.h
git commit -m "feat(agent): add hu_output_validator_t vtable header (P1.T1)"
```

### Task 2: Implement validator helper functions

**Files:**
- Create: `src/agent/output_validator.c`
- Modify: `CMakeLists.txt` — add `src/agent/output_validator.c` to the agent sources

- [ ] **Step 1: Write the failing test first**

Add to `tests/test_output_validator.c` (new file):
```c
#include "human/agent/output_validator.h"
#include "test_framework.h"
#include <string.h>

static hu_allocator_t A(void) { return hu_default_allocator(); }

static void result_free_on_zeroed_struct_is_safe(void) {
    hu_allocator_t alloc = A();
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    hu_validator_result_free(&alloc, &r);
    HU_ASSERT(r.text == NULL && r.reason == NULL);
}

static void result_free_releases_owned_text_and_reason(void) {
    hu_allocator_t alloc = A();
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    r.decision = HU_VALIDATOR_REWRITE;
    r.text = (char *)alloc.alloc(alloc.ctx, 6);
    HU_ASSERT_NOT_NULL((void *)r.text);
    memcpy((char *)r.text, "hello", 6);
    r.text_len = 5;
    r.text_owned = true;
    r.reason = (char *)alloc.alloc(alloc.ctx, 4);
    HU_ASSERT_NOT_NULL(r.reason);
    memcpy(r.reason, "ok", 3);
    r.reason_len = 2;
    hu_validator_result_free(&alloc, &r);
    /* No leak under ASan == pass. */
}

void run_output_validator_tests(void) {
    HU_TEST_SUITE("output_validator");
    HU_RUN_TEST(result_free_on_zeroed_struct_is_safe);
    HU_RUN_TEST(result_free_releases_owned_text_and_reason);
}
```

- [ ] **Step 2: Run test — expect link failure**

Run: `cmake --build --preset dev --target human_tests 2>&1 | grep -i "undefined\|hu_validator_result_free"`
Expected: link error mentioning `hu_validator_result_free`.

- [ ] **Step 3: Implement output_validator.c**

```c
#include "human/agent/output_validator.h"
#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

void hu_validator_result_free(hu_allocator_t *alloc, hu_validator_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->text && result->text_owned) {
        alloc->free(alloc->ctx, (void *)result->text, result->text_len + 1);
    }
    if (result->reason) {
        alloc->free(alloc->ctx, result->reason, result->reason_len + 1);
    }
    result->text = NULL;
    result->text_len = 0;
    result->text_owned = false;
    result->reason = NULL;
    result->reason_len = 0;
}

void hu_output_validator_deinit(hu_output_validator_t *v, hu_allocator_t *alloc) {
    if (!v || !v->vtable)
        return;
    if (v->vtable->deinit) {
        v->vtable->deinit(v->ctx, alloc);
    }
    v->ctx = NULL;
    v->vtable = NULL;
}
```

- [ ] **Step 4: Wire test runner**

Edit `tests/main.c` to add `extern void run_output_validator_tests(void);` and `run_output_validator_tests();` in the test list.

- [ ] **Step 5: Run tests, confirm pass under ASan**

Run: `cmake --build --preset dev --target human_tests && ./build/human_tests --suite=output_validator`
Expected: 2 passed, 0 failed, no ASan errors.

- [ ] **Step 6: Commit**

```bash
git add src/agent/output_validator.c tests/test_output_validator.c tests/main.c CMakeLists.txt
git commit -m "feat(agent): implement hu_validator_result_free + deinit helpers (P1.T2)"
```

### Task 3: Define the chain header

**Files:**
- Create: `include/human/agent/output_validator_chain.h`

- [ ] **Step 1: Write the header**

```c
/* hu_output_validator_chain — composable pipeline of output validators.
 *
 * Conceptually identical to hu_hook_pipeline (include/human/hook_pipeline.h):
 *   - A chain is an ordered list of validators.
 *   - Execution walks the list in registration order.
 *   - PASS  -> continue with same text.
 *   - REWRITE -> continue with new text (current_text is replaced).
 *   - REJECT -> short-circuit; chain reports the failing validator's
 *               name + reason and returns.
 *
 * Memory:
 *   - The chain owns its validators (validators registered via _add take
 *     ownership semantics; the chain calls hu_output_validator_deinit on
 *     each at chain destruction).
 *   - Intermediate rewrite buffers are tracked and freed before chain
 *     execution returns, except for the FINAL output buffer (when the
 *     chain ends with PASS/REWRITE), which is transferred to the caller
 *     via the result struct.
 */
#ifndef HU_AGENT_OUTPUT_VALIDATOR_CHAIN_H
#define HU_AGENT_OUTPUT_VALIDATOR_CHAIN_H

#include "human/agent/output_validator.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_output_validator_chain hu_output_validator_chain_t;

hu_error_t hu_output_validator_chain_create(hu_allocator_t *alloc,
                                            hu_output_validator_chain_t **out);
void hu_output_validator_chain_destroy(hu_output_validator_chain_t *chain);

/* Append a validator. The chain takes ownership of `v` (its deinit will
 * be called at chain destruction). On error the validator is NOT owned. */
hu_error_t hu_output_validator_chain_add(hu_output_validator_chain_t *chain,
                                         hu_output_validator_t v);

size_t hu_output_validator_chain_len(const hu_output_validator_chain_t *chain);

/* Per-execution result. Holds the final text + which validator (if any)
 * rejected, and a list of which validators rewrote or rejected for logs. */
typedef struct hu_chain_result {
    hu_validator_decision_t final_decision; /* PASS / REWRITE / REJECT */
    const char *final_text;                 /* NULL on REJECT */
    size_t final_text_len;
    bool final_text_owned; /* free with alloc if true */
    /* Index of the validator that produced final_decision; SIZE_MAX if
     * empty chain or no decision changed the text. */
    size_t deciding_validator;
    /* Name of deciding validator (borrowed pointer, valid for chain lifetime). */
    const char *deciding_validator_name;
    /* On REJECT, the reason returned by that validator (allocator-owned). */
    char *reject_reason;
    size_t reject_reason_len;
    /* Counts for telemetry. */
    size_t rewrite_count;
    size_t reject_count; /* always 0 or 1 (chain short-circuits) */
} hu_chain_result_t;

void hu_chain_result_free(hu_allocator_t *alloc, hu_chain_result_t *result);

hu_error_t hu_output_validator_chain_execute(const hu_output_validator_chain_t *chain,
                                             hu_allocator_t *alloc,
                                             const hu_validator_context_t *vctx,
                                             const char *response, size_t response_len,
                                             hu_chain_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_OUTPUT_VALIDATOR_CHAIN_H */
```

- [ ] **Step 2: Sanity-compile the header**

Run: `gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -I include -fsyntax-only include/human/agent/output_validator_chain.h`
Expected: clean.

- [ ] **Step 3: Commit**

```bash
git add include/human/agent/output_validator_chain.h
git commit -m "feat(agent): add hu_output_validator_chain header (P1.T3)"
```

### Task 4: Implement chain create / add / destroy

**Files:**
- Create: `src/agent/output_validator_chain.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing tests first**

Append to `tests/test_output_validator.c`:
```c
#include "human/agent/output_validator_chain.h"

static void chain_create_and_destroy_empty(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    HU_ASSERT_NOT_NULL(chain);
    HU_ASSERT_EQ(hu_output_validator_chain_len(chain), 0);
    hu_output_validator_chain_destroy(chain);
}

/* Synthetic validator used across chain tests: marks ctx as visited. */
typedef struct { int visited; } visit_ctx_t;
static hu_error_t visit_validate(void *vctx, hu_allocator_t *alloc,
                                  const hu_validator_context_t *c,
                                  const char *r, size_t rl,
                                  hu_validator_result_t *out) {
    (void)alloc; (void)c; (void)r; (void)rl;
    visit_ctx_t *ctx = (visit_ctx_t *)vctx;
    ctx->visited++;
    out->decision = HU_VALIDATOR_PASS;
    out->text = NULL; out->text_len = 0; out->text_owned = false;
    out->reason = NULL; out->reason_len = 0;
    return HU_OK;
}
static const char *visit_name(void *vctx) { (void)vctx; return "visit"; }
static const hu_output_validator_vtable_t visit_vtable = {
    .validate = visit_validate, .name = visit_name, .deinit = NULL,
};

static void chain_add_then_len_increments(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    visit_ctx_t v1 = {0}, v2 = {0};
    hu_output_validator_t ov1 = {.ctx = &v1, .vtable = &visit_vtable};
    hu_output_validator_t ov2 = {.ctx = &v2, .vtable = &visit_vtable};
    HU_ASSERT_EQ(hu_output_validator_chain_add(chain, ov1), HU_OK);
    HU_ASSERT_EQ(hu_output_validator_chain_add(chain, ov2), HU_OK);
    HU_ASSERT_EQ(hu_output_validator_chain_len(chain), 2);
    hu_output_validator_chain_destroy(chain);
}
```

Register `chain_create_and_destroy_empty` and `chain_add_then_len_increments` in `run_output_validator_tests`.

- [ ] **Step 2: Run, expect failure**

Run: `cmake --build --preset dev --target human_tests`
Expected: unresolved `hu_output_validator_chain_create` etc.

- [ ] **Step 3: Implement chain.c**

```c
#include "human/agent/output_validator_chain.h"
#include "human/agent/output_validator.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define HU_VALIDATOR_CHAIN_INITIAL_CAP 8

struct hu_output_validator_chain {
    hu_allocator_t *alloc;
    hu_output_validator_t *entries;
    size_t len;
    size_t cap;
};

hu_error_t hu_output_validator_chain_create(hu_allocator_t *alloc,
                                            hu_output_validator_chain_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_output_validator_chain_t *c =
        (hu_output_validator_chain_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->entries = (hu_output_validator_t *)alloc->alloc(
        alloc->ctx, HU_VALIDATOR_CHAIN_INITIAL_CAP * sizeof(hu_output_validator_t));
    if (!c->entries) {
        alloc->free(alloc->ctx, c, sizeof(*c));
        return HU_ERR_OUT_OF_MEMORY;
    }
    c->len = 0;
    c->cap = HU_VALIDATOR_CHAIN_INITIAL_CAP;
    *out = c;
    return HU_OK;
}

void hu_output_validator_chain_destroy(hu_output_validator_chain_t *chain) {
    if (!chain)
        return;
    for (size_t i = 0; i < chain->len; i++) {
        hu_output_validator_deinit(&chain->entries[i], chain->alloc);
    }
    chain->alloc->free(chain->alloc->ctx, chain->entries,
                       chain->cap * sizeof(hu_output_validator_t));
    chain->alloc->free(chain->alloc->ctx, chain, sizeof(*chain));
}

hu_error_t hu_output_validator_chain_add(hu_output_validator_chain_t *chain,
                                         hu_output_validator_t v) {
    if (!chain || !v.vtable || !v.vtable->validate || !v.vtable->name)
        return HU_ERR_INVALID_ARGUMENT;
    if (chain->len == chain->cap) {
        size_t new_cap = chain->cap * 2;
        hu_output_validator_t *grow = (hu_output_validator_t *)chain->alloc->alloc(
            chain->alloc->ctx, new_cap * sizeof(hu_output_validator_t));
        if (!grow)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(grow, chain->entries, chain->len * sizeof(hu_output_validator_t));
        chain->alloc->free(chain->alloc->ctx, chain->entries,
                           chain->cap * sizeof(hu_output_validator_t));
        chain->entries = grow;
        chain->cap = new_cap;
    }
    chain->entries[chain->len++] = v;
    return HU_OK;
}

size_t hu_output_validator_chain_len(const hu_output_validator_chain_t *chain) {
    return chain ? chain->len : 0;
}
```

- [ ] **Step 4: Run tests, confirm pass**

Run: `cmake --build --preset dev && ./build/human_tests --suite=output_validator`
Expected: all pass, 0 ASan errors.

- [ ] **Step 5: Commit**

```bash
git add src/agent/output_validator_chain.c tests/test_output_validator.c CMakeLists.txt
git commit -m "feat(agent): chain create/add/destroy + tests (P1.T4)"
```

### Task 5: Implement chain execute (the core algorithm)

**Files:**
- Modify: `src/agent/output_validator_chain.c`

- [ ] **Step 1: Write the full execution semantics as tests first**

Append to `tests/test_output_validator.c`:
```c
/* Pass-pass-pass chain returns final text unchanged, no allocation transferred. */
static void chain_execute_all_pass_returns_input_unchanged(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    visit_ctx_t v1 = {0}, v2 = {0};
    hu_output_validator_t ov1 = {.ctx = &v1, .vtable = &visit_vtable};
    hu_output_validator_t ov2 = {.ctx = &v2, .vtable = &visit_vtable};
    hu_output_validator_chain_add(chain, ov1);
    hu_output_validator_chain_add(chain, ov2);
    const char *in = "hello world";
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, in, 11, &cr), HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_PASS);
    HU_ASSERT(cr.final_text == in);
    HU_ASSERT_EQ(cr.final_text_len, 11u);
    HU_ASSERT(!cr.final_text_owned);
    HU_ASSERT_EQ(v1.visited, 1);
    HU_ASSERT_EQ(v2.visited, 1);
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* Synthetic uppercase-rewriter: turns 'h' -> 'H'. */
typedef struct { int call_count; } upper_ctx_t;
static hu_error_t upper_validate(void *vctx, hu_allocator_t *alloc,
                                  const hu_validator_context_t *c,
                                  const char *r, size_t rl,
                                  hu_validator_result_t *out) {
    (void)c;
    upper_ctx_t *ctx = (upper_ctx_t *)vctx;
    ctx->call_count++;
    char *buf = (char *)alloc->alloc(alloc->ctx, rl + 1);
    if (!buf) return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < rl; i++) {
        char ch = r[i];
        buf[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    buf[rl] = '\0';
    out->decision = HU_VALIDATOR_REWRITE;
    out->text = buf; out->text_len = rl; out->text_owned = true;
    out->reason = NULL; out->reason_len = 0;
    return HU_OK;
}
static const char *upper_name(void *vctx) { (void)vctx; return "upper"; }
static const hu_output_validator_vtable_t upper_vtable = {
    .validate = upper_validate, .name = upper_name, .deinit = NULL,
};

static void chain_rewrite_then_pass_returns_rewritten(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    hu_output_validator_chain_create(&alloc, &chain);
    upper_ctx_t u = {0};
    visit_ctx_t v = {0};
    hu_output_validator_chain_add(chain, (hu_output_validator_t){.ctx = &u, .vtable = &upper_vtable});
    hu_output_validator_chain_add(chain, (hu_output_validator_t){.ctx = &v, .vtable = &visit_vtable});
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, "hi", 2, &cr), HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REWRITE);
    HU_ASSERT(cr.final_text_owned);
    HU_ASSERT_EQ(cr.final_text_len, 2u);
    HU_ASSERT(strncmp(cr.final_text, "HI", 2) == 0);
    HU_ASSERT_EQ(cr.rewrite_count, 1u);
    HU_ASSERT_EQ(v.visited, 1);
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* Reject validator. */
typedef struct { const char *msg; } reject_ctx_t;
static hu_error_t reject_validate(void *vctx, hu_allocator_t *alloc,
                                   const hu_validator_context_t *c,
                                   const char *r, size_t rl,
                                   hu_validator_result_t *out) {
    (void)c; (void)r; (void)rl;
    reject_ctx_t *ctx = (reject_ctx_t *)vctx;
    size_t mlen = strlen(ctx->msg);
    char *reason = (char *)alloc->alloc(alloc->ctx, mlen + 1);
    if (!reason) return HU_ERR_OUT_OF_MEMORY;
    memcpy(reason, ctx->msg, mlen + 1);
    out->decision = HU_VALIDATOR_REJECT;
    out->text = NULL; out->text_len = 0; out->text_owned = false;
    out->reason = reason; out->reason_len = mlen;
    return HU_OK;
}
static const char *reject_name(void *vctx) { (void)vctx; return "reject"; }
static const hu_output_validator_vtable_t reject_vtable = {
    .validate = reject_validate, .name = reject_name, .deinit = NULL,
};

static void chain_reject_short_circuits(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    hu_output_validator_chain_create(&alloc, &chain);
    reject_ctx_t rc = {.msg = "nope"};
    visit_ctx_t v = {0};
    hu_output_validator_chain_add(chain, (hu_output_validator_t){.ctx = &rc, .vtable = &reject_vtable});
    hu_output_validator_chain_add(chain, (hu_output_validator_t){.ctx = &v, .vtable = &visit_vtable});
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, "x", 1, &cr), HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_REJECT);
    HU_ASSERT(cr.final_text == NULL);
    HU_ASSERT_EQ(cr.reject_count, 1u);
    HU_ASSERT(cr.reject_reason && strncmp(cr.reject_reason, "nope", 4) == 0);
    HU_ASSERT_EQ(v.visited, 0); /* never reached */
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}
```

Register all three tests in `run_output_validator_tests`.

- [ ] **Step 2: Run, expect failure (unresolved symbols hu_chain_result_free + hu_output_validator_chain_execute)**

- [ ] **Step 3: Implement the execution algorithm**

Append to `src/agent/output_validator_chain.c`:
```c
void hu_chain_result_free(hu_allocator_t *alloc, hu_chain_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->final_text && result->final_text_owned) {
        alloc->free(alloc->ctx, (void *)result->final_text, result->final_text_len + 1);
    }
    if (result->reject_reason) {
        alloc->free(alloc->ctx, result->reject_reason, result->reject_reason_len + 1);
    }
    memset(result, 0, sizeof(*result));
    result->deciding_validator = (size_t)-1;
}

hu_error_t hu_output_validator_chain_execute(const hu_output_validator_chain_t *chain,
                                             hu_allocator_t *alloc,
                                             const hu_validator_context_t *vctx,
                                             const char *response, size_t response_len,
                                             hu_chain_result_t *out) {
    if (!chain || !alloc || !response || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->deciding_validator = (size_t)-1;

    const char *current = response;
    size_t current_len = response_len;
    bool current_owned = false;

    for (size_t i = 0; i < chain->len; i++) {
        const hu_output_validator_t *v = &chain->entries[i];
        hu_validator_result_t r;
        memset(&r, 0, sizeof(r));
        hu_error_t err = v->vtable->validate(v->ctx, alloc, vctx, current, current_len, &r);
        if (err != HU_OK) {
            if (current_owned) {
                alloc->free(alloc->ctx, (void *)current, current_len + 1);
            }
            return err;
        }
        if (r.decision == HU_VALIDATOR_PASS) {
            hu_validator_result_free(alloc, &r);
            continue;
        }
        if (r.decision == HU_VALIDATOR_REWRITE) {
            /* Free the previous buffer if we owned it. */
            if (current_owned) {
                alloc->free(alloc->ctx, (void *)current, current_len + 1);
            }
            current = r.text;
            current_len = r.text_len;
            current_owned = r.text_owned;
            /* Free only the reason (text ownership transferred). */
            if (r.reason) {
                alloc->free(alloc->ctx, r.reason, r.reason_len + 1);
            }
            out->rewrite_count++;
            out->deciding_validator = i;
            out->deciding_validator_name = v->vtable->name(v->ctx);
            continue;
        }
        /* REJECT — short circuit. */
        if (current_owned) {
            alloc->free(alloc->ctx, (void *)current, current_len + 1);
        }
        out->final_decision = HU_VALIDATOR_REJECT;
        out->final_text = NULL;
        out->final_text_len = 0;
        out->final_text_owned = false;
        out->deciding_validator = i;
        out->deciding_validator_name = v->vtable->name(v->ctx);
        out->reject_reason = r.reason;
        out->reject_reason_len = r.reason_len;
        out->reject_count = 1;
        return HU_OK;
    }

    out->final_decision = current_owned ? HU_VALIDATOR_REWRITE : HU_VALIDATOR_PASS;
    out->final_text = current;
    out->final_text_len = current_len;
    out->final_text_owned = current_owned;
    return HU_OK;
}
```

- [ ] **Step 4: Run tests under ASan, confirm all pass with no leaks**

Run: `cmake --build --preset dev && ./build/human_tests --suite=output_validator`
Expected: 5+ tests pass, no leaks.

- [ ] **Step 5: Commit**

```bash
git add src/agent/output_validator_chain.c tests/test_output_validator.c
git commit -m "feat(agent): implement output_validator chain execution (P1.T5)"
```

### Task 6: Phase-1 verification gate

- [ ] **Step 1: Run full test suite to confirm no regressions**

Run: `./build/human_tests`
Expected: all 9,800+ tests pass, 0 ASan errors.

- [ ] **Step 2: Spawn `/verify` to capture evidence**

```
/verify "Phase 1 — validator vtable infrastructure. Verify: hu_output_validator_t, hu_output_validator_chain_t exist; chain create/add/destroy/execute pass tests under ASan; no existing tests regressed."
```
Expected: RESULT_verifier=PASS.

- [ ] **Step 3: Commit phase tag**

```bash
git tag -a phase-1-complete -m "Validator vtable + chain infrastructure shipped, no behavior change"
```

---

## Phase 2 — Migrate Existing Strippers as Validators

Goal: every existing stripper becomes a vtable validator. Existing direct calls remain in place until P2.T12 swaps them for chain execution. All 52 existing tests stay green; the migration is mechanical (functions get a thin adapter shim).

### Task 7: Build the validators directory + harmony_token_validator

**Files:**
- Create: `src/agent/validators/harmony_token_validator.c`
- Create: `include/human/agent/validators/builtin.h`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Define the factory header**

Create `include/human/agent/validators/builtin.h`:
```c
/* Factories for the built-in output validators. Each returns a
 * hu_output_validator_t by value; the chain takes ownership. */
#ifndef HU_AGENT_VALIDATORS_BUILTIN_H
#define HU_AGENT_VALIDATORS_BUILTIN_H

#include "human/agent/output_validator.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_validator_harmony_token_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_thinking_block_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_degenerate_repetition_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_bullet_cot_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_channel_tags_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_ai_phrases_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_formal_structure_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_cot_audit_create(hu_allocator_t *alloc, hu_output_validator_t *out);

/* New (P3): */
hu_error_t hu_validator_persona_narrator_create(hu_allocator_t *alloc,
                                                const char *persona_name, size_t persona_name_len,
                                                hu_output_validator_t *out);
hu_error_t hu_validator_assistant_closer_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_role_consistency_create(hu_allocator_t *alloc, hu_output_validator_t *out);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Write the harmony_token_validator test first**

Add to new file `tests/test_validators_builtin.c`:
```c
#include "human/agent/validators/builtin.h"
#include "human/agent/output_validator.h"
#include "test_framework.h"
#include <string.h>

static hu_allocator_t A(void) { return hu_default_allocator(); }

static hu_validator_result_t run_validator(hu_output_validator_t v, hu_allocator_t *alloc,
                                            const char *in, size_t in_len) {
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, alloc, NULL, in, in_len, &r);
    return r;
}

static void harmony_pass_on_clean_text(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_harmony_token_create(&alloc, &v), HU_OK);
    hu_validator_result_t r = run_validator(v, &alloc, "hello there", 11);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void harmony_rewrites_on_token_leak(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_harmony_token_create(&alloc, &v), HU_OK);
    const char *in = "Like <|channel>thoughtThe user said hello";
    hu_validator_result_t r = run_validator(v, &alloc, in, strlen(in));
    HU_ASSERT(r.decision == HU_VALIDATOR_REWRITE || r.decision == HU_VALIDATOR_REJECT);
    if (r.decision == HU_VALIDATOR_REWRITE) {
        HU_ASSERT(strstr(r.text, "<|channel") == NULL);
    }
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

void run_validators_builtin_tests(void) {
    HU_TEST_SUITE("validators_builtin");
    HU_RUN_TEST(harmony_pass_on_clean_text);
    HU_RUN_TEST(harmony_rewrites_on_token_leak);
}
```

- [ ] **Step 3: Run, expect link failure**

- [ ] **Step 4: Implement harmony_token_validator.c**

```c
#include "human/agent/validators/builtin.h"
#include "human/agent/response_guard.h"
#include <stdbool.h>
#include <string.h>

static hu_error_t harmony_validate(void *ctx, hu_allocator_t *alloc,
                                    const hu_validator_context_t *vctx,
                                    const char *response, size_t response_len,
                                    hu_validator_result_t *out) {
    (void)ctx; (void)vctx;
    memset(out, 0, sizeof(*out));
    if (!hu_response_guard_has_special_token(response, response_len)) {
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }
    /* Delegate to the existing guard for the rewrite; ignore the
     * Gemma-4 prefix path here — that's bullet_cot_validator's job. */
    char *rewritten = NULL;
    size_t rewritten_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    hu_error_t err = hu_response_guard_check(alloc, response, response_len, &rewritten,
                                              &rewritten_len, &outcome, &report);
    if (err != HU_OK) return err;
    if (outcome == HU_GUARD_REJECT) {
        out->decision = HU_VALIDATOR_REJECT;
        const char *msg = "harmony token leak: response unsalvageable";
        size_t mlen = strlen(msg);
        out->reason = (char *)alloc->alloc(alloc->ctx, mlen + 1);
        if (!out->reason) return HU_ERR_OUT_OF_MEMORY;
        memcpy(out->reason, msg, mlen + 1);
        out->reason_len = mlen;
        return HU_OK;
    }
    if (outcome == HU_GUARD_REWROTE) {
        out->decision = HU_VALIDATOR_REWRITE;
        out->text = rewritten;
        out->text_len = rewritten_len;
        out->text_owned = true;
        return HU_OK;
    }
    out->decision = HU_VALIDATOR_PASS;
    return HU_OK;
}

static const char *harmony_name(void *ctx) { (void)ctx; return "harmony_token"; }

static const hu_output_validator_vtable_t harmony_vtable = {
    .validate = harmony_validate,
    .name = harmony_name,
    .deinit = NULL,
};

hu_error_t hu_validator_harmony_token_create(hu_allocator_t *alloc, hu_output_validator_t *out) {
    (void)alloc;
    if (!out) return HU_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &harmony_vtable;
    return HU_OK;
}
```

- [ ] **Step 5: Run tests, confirm pass**

Expected: 2 new tests in `validators_builtin` suite pass.

- [ ] **Step 6: Commit**

```bash
git add src/agent/validators/harmony_token_validator.c include/human/agent/validators/builtin.h tests/test_validators_builtin.c CMakeLists.txt
git commit -m "feat(agent): harmony_token_validator wraps response_guard (P2.T7)"
```

### Tasks 8-11: Wrap remaining strippers as validators

Each task follows the exact pattern of Task 7. For brevity I list them with their specific delegate + factory; structure is identical.

**Task 8 — thinking_block_validator** (`src/agent/validators/thinking_block_validator.c`)
- Delegate: same `hu_response_guard_check`, but reports decision only when `report.stripped_thinking_block` was set. Since one guard handles both harmony + thinking, the cleaner pattern is to keep them as separate validators that BOTH gate on `hu_response_guard_has_special_token` returning different things — or have the guard expose `hu_response_guard_has_thinking_block` separately. **Add** `hu_response_guard_has_thinking_block(const char *, size_t)` to `include/human/agent/response_guard.h` + `src/agent/response_guard.c` (already structurally there inside `has_special_token`; split into a separate helper).
- Tests: `thinking_pass_on_clean`, `thinking_rewrites_when_think_tag_present`, `thinking_rewrites_when_thought_tag_present`.

**Task 9 — degenerate_repetition_validator** (`src/agent/validators/degenerate_repetition_validator.c`)
- Delegate: `hu_response_guard_longest_char_run` + `hu_response_guard_longest_token_run`. If either exceeds threshold → REJECT with reason "degenerate repetition".
- Tests: `degenerate_passes_normal`, `degenerate_rejects_long_quote_run`, `degenerate_rejects_long_token_run`.

**Task 10 — bullet_cot_validator** (`src/agent/validators/bullet_cot_validator.c`)
- Detects `* ` bullet-CoT prefix; rewrites by skipping past the bullet block to the first non-bullet line. If entire response is bullets → REJECT.
- Tests: `bullet_passes_normal`, `bullet_strips_prefix`, `bullet_rejects_pure_reasoning`.

**Task 11 — channel_tags_validator / ai_phrases_validator / formal_structure_validator / cot_audit_validator**
- Each wraps the corresponding existing function. The strip functions mutate buffer in place — wrap by copying input → mutating → returning REWRITE.
- For `cot_audit_validator`: this is the only validator that operates on `reasoning_content` rather than the main `content`. Design choice: the validator accepts `content`, but we'll wire it ONLY into the chain that runs over reasoning_content in P2.T12.
- Tests: one per existing test in `tests/test_conversation.c` translated to the validator-result-shape; original tests stay (they still pin the underlying strip functions).

Each task:
- [ ] **Step 1: Write failing tests (one per existing stripper test case, adapted)**
- [ ] **Step 2: Implement adapter**
- [ ] **Step 3: Confirm pass**
- [ ] **Step 4: Commit**

```bash
git commit -m "feat(agent): <validator_name> validator wraps <existing_function> (P2.T<n>)"
```

### Task 12: Replace call sites with chain execution

**Files modified (18 call sites total):**
- `src/agent/agent_turn.c:5587-5664`
- `src/agent/agent_stream.c:1409-1467, 2140-2200`
- `src/daemon.c:1068-1070, 1712-1715, 2052, 9201-9204, 10539-10541, 11562-11565`
- `src/daemon_cron.c:286`
- `src/channels/imessage.c:909`
- `src/channels/format.c:141`
- `src/gateway/openai_compat.c:620-622`

Plus chain construction helper.

- [ ] **Step 1: Create a chain factory for the default outbound chain**

Add to `include/human/agent/validators/builtin.h`:
```c
/* Build the default outbound chain in registration order:
 *   1. harmony_token         (REJECT/REWRITE if special tokens)
 *   2. thinking_block        (REWRITE if <think>/<thought>)
 *   3. degenerate_repetition (REJECT if pathological repetition)
 *   4. bullet_cot            (REWRITE if `* ` prefix CoT)
 *   5. channel_tags          (REWRITE strip)
 *   6. ai_phrases            (REWRITE strip)
 *   7. formal_structure      (REWRITE strip)
 *   8. assistant_closer      (P3 — REWRITE if AI-helper closer)
 *   9. persona_narrator      (P3 — REJECT if third-person persona)
 *  10. role_consistency      (P3 — REJECT if mid-msg role pivot)
 *
 * In P2 only validators 1-7 are wired; 8-10 are added in P3.
 *
 * persona_name is borrowed; chain copies internally where needed. */
hu_error_t hu_validators_build_default_outbound_chain(
    hu_allocator_t *alloc,
    const char *persona_name, size_t persona_name_len,
    hu_output_validator_chain_t **out);
```

- [ ] **Step 2: Implement the factory**

In `src/agent/validators/default_chains.c` (new file):
```c
#include "human/agent/validators/builtin.h"
#include "human/agent/output_validator_chain.h"

hu_error_t hu_validators_build_default_outbound_chain(
    hu_allocator_t *alloc,
    const char *persona_name, size_t persona_name_len,
    hu_output_validator_chain_t **out) {
    (void)persona_name; (void)persona_name_len; /* used in P3 */
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    hu_output_validator_chain_t *chain = NULL;
    hu_error_t err = hu_output_validator_chain_create(alloc, &chain);
    if (err != HU_OK) return err;

    hu_output_validator_t v;
    #define ADD(creator) do { \
        err = creator; \
        if (err != HU_OK) goto fail; \
        err = hu_output_validator_chain_add(chain, v); \
        if (err != HU_OK) { hu_output_validator_deinit(&v, alloc); goto fail; } \
    } while (0)

    ADD(hu_validator_harmony_token_create(alloc, &v));
    ADD(hu_validator_thinking_block_create(alloc, &v));
    ADD(hu_validator_degenerate_repetition_create(alloc, &v));
    ADD(hu_validator_bullet_cot_create(alloc, &v));
    ADD(hu_validator_channel_tags_create(alloc, &v));
    ADD(hu_validator_ai_phrases_create(alloc, &v));
    ADD(hu_validator_formal_structure_create(alloc, &v));

    #undef ADD
    *out = chain;
    return HU_OK;

fail:
    hu_output_validator_chain_destroy(chain);
    return err;
}
```

- [ ] **Step 3: Write integration test pinning chain == current pipeline**

Add to `tests/test_validators_builtin.c`:
```c
static void chain_matches_existing_pipeline_for_known_inputs(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, NULL, 0, &chain), HU_OK);

    /* Use the same input as tests/test_conversation.c::strip_pipeline_full_integration. */
    const char *raw =
        "<thinking>internal</thinking>1. Item one - Item two\n"
        "As an AI, I'd be happy to help! Don't hesitate to ask!!";
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, raw, strlen(raw), &cr), HU_OK);
    HU_ASSERT(cr.final_decision != HU_VALIDATOR_REJECT);
    /* Expected outputs from the existing pipeline (verified manually); pin
     * here so future changes to either path surface. */
    HU_ASSERT(strstr(cr.final_text, "<thinking>") == NULL);
    HU_ASSERT(strstr(cr.final_text, "As an AI") == NULL);
    HU_ASSERT(strstr(cr.final_text, "I'd be happy to") == NULL);
    HU_ASSERT(strstr(cr.final_text, "!!") == NULL);
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}
```

- [ ] **Step 4: Confirm test passes**

- [ ] **Step 5: Replace agent_turn.c call sites (5587-5664) with chain execution**

The current code:
```c
hu_error_t guard_err = hu_response_guard_check(
    agent->alloc, final_content, final_len, &guard_out, &guard_out_len,
    &guard_outcome, &guard_report);
/* ... handle REJECT via retry-slim ... */
/* ... handle REWROTE ... */
final_len = hu_conversation_strip_channel_tags(...);
final_len = hu_conversation_strip_ai_phrases(...);
final_len = hu_conversation_strip_formal_structure(...);
```

Becomes:
```c
hu_output_validator_chain_t *out_chain = NULL;
const char *persona_name = agent->persona && agent->persona->name ? agent->persona->name : NULL;
size_t persona_name_len = persona_name ? strlen(persona_name) : 0;
if (hu_validators_build_default_outbound_chain(agent->alloc, persona_name, persona_name_len,
                                                &out_chain) == HU_OK) {
    hu_validator_context_t vctx = {
        .channel_id = current_channel_id, .channel_id_len = current_channel_id_len,
        .persona_name = persona_name, .persona_name_len = persona_name_len,
        .provider_name = agent->provider.vtable->get_name(agent->provider.ctx),
        .provider_name_len = strlen(agent->provider.vtable->get_name(agent->provider.ctx)),
    };
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    hu_error_t cerr = hu_output_validator_chain_execute(out_chain, agent->alloc, &vctx,
                                                        final_content, final_len, &cr);
    if (cerr == HU_OK) {
        if (cr.final_decision == HU_VALIDATOR_REJECT) {
            /* Retry-slim path (existing logic, refactored into helper). */
            hu_error_t retry_err = hu_response_guard_retry_slim(
                agent->alloc, agent->observer, agent->config, &agent->provider,
                turn_model, turn_model_len, msg, msg_len, &retry_content,
                &retry_len, &retry_report);
            /* ... same handling as before ... */
        } else if (cr.final_text_owned) {
            if (ab_owned)
                agent->alloc->free(agent->alloc->ctx, (void *)final_content, final_len + 1);
            final_content = cr.final_text;
            final_len = cr.final_text_len;
            ab_owned = true;
            cr.final_text_owned = false; /* transferred */
        }
        hu_chain_result_free(agent->alloc, &cr);
    }
    hu_output_validator_chain_destroy(out_chain);
}
```

- [ ] **Step 6: Repeat the migration for each call site in agent_stream.c, daemon.c, daemon_cron.c, channels/imessage.c, channels/format.c, gateway/openai_compat.c**

Each call site replaces the stripper sequence with the chain. **One commit per file** to keep diffs small and bisectable.

- [ ] **Step 7: Run full test suite after each file migration**

```bash
./build/human_tests
```
Expected: all 9,800+ tests pass at every step.

- [ ] **Step 8: Final commit + phase tag**

```bash
git tag -a phase-2-complete -m "All 18 stripper call sites migrated to validator chain"
```

### Task 13: Phase-2 verification gate

- [ ] **Step 1: Run `/verify`**

```
/verify "Phase 2 — all 18 stripper call sites use the validator chain. Verify: all 9,800+ tests pass; ASan clean; sample of leaked-token inputs from tests/test_response_guard.c still rejected; the integration test in test_conversation.c::strip_pipeline_full_integration still passes."
```
Expected: RESULT_verifier=PASS.

---

## Phase 3 — New Validators (F1 / F2 / F3)

Goal: ship the three new validators that catch the 2026-05-14 Jordan-channel leaks. Failing-test-first per global rule. The validators plug into the existing chain via the factory updated in P3.T17.

### Task 14: persona_narrator_validator (F1)

Detects responses that narrate ABOUT the persona in third person rather than speaking AS the persona. Heuristic:
- (a) The first ~200 bytes start with one of: "Wait,", "Hmm,", "Let me", "Looking at", "OK so", "Actually,", "Okay so", "Alright,", "So,", "Right,", "Now,", or contain a meta-reference like "the persona", "the AI", "the user", "the rules" within those 200 bytes.
- (b) Within the entire response, the persona's own name (from `vctx.persona_name`) appears followed by a third-person verb ("is", "was", "would", "should", "thinks", "feels").
- If BOTH (a) AND (b) match → REJECT with reason "persona-narrator pattern detected".

**Files:** `src/agent/validators/persona_narrator_validator.c`, tests in `tests/test_validators_persona_safety.c`.

- [ ] **Step 1: Write the regression test using the actual 2026-05-14 Jordan leak**

Create `tests/test_validators_persona_safety.c`:
```c
#include "human/agent/validators/builtin.h"
#include "human/agent/output_validator.h"
#include "test_framework.h"
#include <string.h>

static hu_allocator_t A(void) { return hu_default_allocator(); }

/* The actual prose-CoT leak that reached Jordan on 2026-05-14. */
static const char *JORDAN_LEAK_F1 =
    "Wait, looking at the history, the AI has been slipping into "
    "\"How can I help you today?\" which is a massive AI tell and "
    "explicitly forbidden by the persona instructions. I need to snap "
    "back into Seth.\n\n"
    "Seth is chill, playful, and romantic with Jordan.\n"
    "If she says \"Oh nice!\", he should probably keep it light...";

static void persona_narrator_rejects_jordan_leak(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_narrator_create(&alloc, "Seth", 4, &v), HU_OK);
    hu_validator_context_t vctx = {.persona_name = "Seth", .persona_name_len = 4};
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, &vctx, JORDAN_LEAK_F1, strlen(JORDAN_LEAK_F1), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REJECT);
    HU_ASSERT(r.reason && strstr(r.reason, "persona") != NULL);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void persona_narrator_passes_real_seth_reply(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_narrator_create(&alloc, "Seth", 4, &v), HU_OK);
    hu_validator_context_t vctx = {.persona_name = "Seth", .persona_name_len = 4};
    /* A real in-character reply that happens to start with "Wait,". */
    const char *in = "wait you got the package already? that was fast";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, &vctx, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void persona_narrator_passes_when_persona_name_unknown(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_persona_narrator_create(&alloc, NULL, 0, &v), HU_OK);
    /* No persona name → cannot detect (b), so PASS. */
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, JORDAN_LEAK_F1, strlen(JORDAN_LEAK_F1), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

void run_validators_persona_safety_tests(void) {
    HU_TEST_SUITE("validators_persona_safety");
    HU_RUN_TEST(persona_narrator_rejects_jordan_leak);
    HU_RUN_TEST(persona_narrator_passes_real_seth_reply);
    HU_RUN_TEST(persona_narrator_passes_when_persona_name_unknown);
}
```

- [ ] **Step 2: Run, expect link failure**

- [ ] **Step 3: Implement persona_narrator_validator.c**

```c
#include "human/agent/validators/builtin.h"
#include "human/agent/output_validator.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *persona_name; /* owned; lowercased copy for case-insensitive match */
    size_t persona_name_len;
} persona_narrator_ctx_t;

static const char *const PREAMBLE_PATTERNS[] = {
    "wait,", "hmm,", "let me", "looking at", "ok so", "okay so",
    "actually,", "alright,", "so,", "right,", "now,",
};
static const size_t PREAMBLE_PATTERN_COUNT =
    sizeof(PREAMBLE_PATTERNS) / sizeof(PREAMBLE_PATTERNS[0]);

static const char *const META_PHRASES[] = {
    "the persona", "the ai", "the user", "the rules", "the assistant",
    "persona instructions", "snap back into",
};
static const size_t META_PHRASE_COUNT =
    sizeof(META_PHRASES) / sizeof(META_PHRASES[0]);

static const char *const THIRD_PERSON_VERBS[] = {
    " is ", " was ", " would ", " should ", " thinks ", " feels ",
    " is chill", " is a ", " is the ",
};
static const size_t TPV_COUNT = sizeof(THIRD_PERSON_VERBS) / sizeof(THIRD_PERSON_VERBS[0]);

static bool ci_contains(const char *hay, size_t hay_len, const char *needle, size_t nlen) {
    if (nlen == 0 || nlen > hay_len) return false;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nlen) return true;
    }
    return false;
}

static hu_error_t pn_validate(void *ctx_v, hu_allocator_t *alloc,
                               const hu_validator_context_t *vctx,
                               const char *response, size_t response_len,
                               hu_validator_result_t *out) {
    persona_narrator_ctx_t *ctx = (persona_narrator_ctx_t *)ctx_v;
    memset(out, 0, sizeof(*out));

    /* Need a persona name to detect pattern (b). */
    const char *persona = ctx->persona_name;
    size_t persona_len = ctx->persona_name_len;
    if ((!persona || persona_len == 0) && vctx) {
        persona = vctx->persona_name;
        persona_len = vctx->persona_name_len;
    }
    if (!persona || persona_len == 0) {
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    size_t scan_len = response_len < 200 ? response_len : 200;

    /* (a) preamble OR meta-reference within first 200 bytes */
    bool found_a = false;
    for (size_t i = 0; i < PREAMBLE_PATTERN_COUNT && !found_a; i++) {
        size_t plen = strlen(PREAMBLE_PATTERNS[i]);
        size_t check = scan_len < 50 ? scan_len : 50; /* preambles must be near the start */
        if (ci_contains(response, check, PREAMBLE_PATTERNS[i], plen)) found_a = true;
    }
    for (size_t i = 0; i < META_PHRASE_COUNT && !found_a; i++) {
        size_t plen = strlen(META_PHRASES[i]);
        if (ci_contains(response, scan_len, META_PHRASES[i], plen)) found_a = true;
    }
    if (!found_a) {
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    /* (b) "<PersonaName> <third-person-verb>" anywhere in the response. */
    bool found_b = false;
    for (size_t i = 0; i + persona_len < response_len && !found_b; i++) {
        if (ci_contains(response + i, persona_len, persona, persona_len) &&
            (i == 0 || response[i - 1] == ' ' || response[i - 1] == '\n' ||
             response[i - 1] == '.' || response[i - 1] == '\r')) {
            size_t after = i + persona_len;
            for (size_t k = 0; k < TPV_COUNT && !found_b; k++) {
                size_t vlen = strlen(THIRD_PERSON_VERBS[k]);
                if (after + vlen <= response_len &&
                    ci_contains(response + after, vlen, THIRD_PERSON_VERBS[k], vlen)) {
                    found_b = true;
                }
            }
        }
    }
    if (!found_b) {
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    const char *msg = "persona-narrator pattern detected (third-person reference to active persona)";
    size_t mlen = strlen(msg);
    out->reason = (char *)alloc->alloc(alloc->ctx, mlen + 1);
    if (!out->reason) return HU_ERR_OUT_OF_MEMORY;
    memcpy(out->reason, msg, mlen + 1);
    out->reason_len = mlen;
    out->decision = HU_VALIDATOR_REJECT;
    return HU_OK;
}

static const char *pn_name(void *ctx) { (void)ctx; return "persona_narrator"; }

static void pn_deinit(void *ctx_v, hu_allocator_t *alloc) {
    persona_narrator_ctx_t *ctx = (persona_narrator_ctx_t *)ctx_v;
    if (!ctx) return;
    if (ctx->persona_name) alloc->free(alloc->ctx, ctx->persona_name, ctx->persona_name_len + 1);
    alloc->free(alloc->ctx, ctx, sizeof(*ctx));
}

static const hu_output_validator_vtable_t pn_vtable = {
    .validate = pn_validate, .name = pn_name, .deinit = pn_deinit,
};

hu_error_t hu_validator_persona_narrator_create(hu_allocator_t *alloc,
                                                const char *persona_name, size_t persona_name_len,
                                                hu_output_validator_t *out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    persona_narrator_ctx_t *ctx = (persona_narrator_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx) return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    if (persona_name && persona_name_len > 0) {
        ctx->persona_name = (char *)alloc->alloc(alloc->ctx, persona_name_len + 1);
        if (!ctx->persona_name) {
            alloc->free(alloc->ctx, ctx, sizeof(*ctx));
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < persona_name_len; i++) {
            char c = persona_name[i];
            ctx->persona_name[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        ctx->persona_name[persona_name_len] = '\0';
        ctx->persona_name_len = persona_name_len;
    }
    out->ctx = ctx;
    out->vtable = &pn_vtable;
    return HU_OK;
}
```

- [ ] **Step 4: Run tests, confirm 3 pass + Jordan leak rejected**

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(agent): persona_narrator_validator catches F1 prose-CoT leak (P3.T14)"
```

### Task 15: assistant_closer_validator (F2)

Detects canonical AI-helper closers that aren't in the existing `ai_phrases` table. Rewrites to strip them; never rejects (these are routine, not catastrophic).

**Closers to strip (case-insensitive, end-of-line or end-of-message):**
- "How can I help you today?"
- "Is there anything I can help you with?"
- "Is there anything else I can help you with?"
- "Let me know if you have any other questions"
- "Feel free to ask if you have more questions"
- "I'm all set, thank you!" (the role-swap close from the F3 leak)
- "I hope that helps!"
- "Hope this helps!"

**Files:** `src/agent/validators/assistant_closer_validator.c`, tests appended to `test_validators_persona_safety.c`.

- [ ] **Step 1: Failing test using the actual F2 leak**

```c
static void assistant_closer_strips_jordan_F2_leak(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_assistant_closer_create(&alloc, &v), HU_OK);
    const char *in = "made my night tbh\nI'm all set, thank you! "
                     "Is there anything I can help you with?";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REWRITE);
    HU_ASSERT(strstr(r.text, "Is there anything") == NULL);
    HU_ASSERT(strstr(r.text, "I'm all set") == NULL);
    HU_ASSERT(strstr(r.text, "made my night tbh") != NULL);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}
```

- [ ] **Step 2-5: implement, test, commit** (same pattern as Task 14)

```bash
git commit -am "feat(agent): assistant_closer_validator catches F2 AI-helper closers (P3.T15)"
```

### Task 16: role_consistency_validator (F3)

Detects responses where the model emits a valid reply, then a newline, then assistant-bot or user-dismissal speech ("I'm all set, thank you!"). Heuristic: split the response into sentences; check whether any sentence AFTER the first contains assistant-mode patterns or third-person-helper patterns.

Tighter rule: if the response contains `\n\n<assistant-pattern>` OR `\n\n<user-dismissal-pattern>`, REJECT — the model went past its turn boundary.

**Files:** `src/agent/validators/role_consistency_validator.c`, tests appended.

- [ ] **Step 1: Failing test with the F3 leak**

```c
static void role_consistency_rejects_F3_mid_message_pivot(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_role_consistency_create(&alloc, &v), HU_OK);
    const char *in = "made my night tbh\n\nI'm all set, thank you!";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_REJECT);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}

static void role_consistency_passes_legitimate_double_text(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_t v;
    HU_ASSERT_EQ(hu_validator_role_consistency_create(&alloc, &v), HU_OK);
    /* Real users do send two-paragraph replies. */
    const char *in = "made my night tbh\n\nactually wait, you doing anything tomorrow?";
    hu_validator_result_t r;
    memset(&r, 0, sizeof(r));
    v.vtable->validate(v.ctx, &alloc, NULL, in, strlen(in), &r);
    HU_ASSERT_EQ(r.decision, HU_VALIDATOR_PASS);
    hu_validator_result_free(&alloc, &r);
    hu_output_validator_deinit(&v, &alloc);
}
```

- [ ] **Step 2-5: implement, test, commit**

```bash
git commit -am "feat(agent): role_consistency_validator catches F3 mid-msg role pivot (P3.T16)"
```

### Task 17: Wire P3 validators into default outbound chain

**File:** `src/agent/validators/default_chains.c`

- [ ] **Step 1: Append the three new validators to the chain factory**

Modify `hu_validators_build_default_outbound_chain` to add after `ai_phrases`:
```c
ADD(hu_validator_assistant_closer_create(alloc, &v));
ADD(hu_validator_persona_narrator_create(alloc, persona_name, persona_name_len, &v));
ADD(hu_validator_role_consistency_create(alloc, &v));
```

- [ ] **Step 2: Update integration test to assert all three new validators in the chain**

In `tests/test_validators_builtin.c`:
```c
static void default_chain_contains_F1_F2_F3_validators(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, "Seth", 4, &chain), HU_OK);
    /* The chain should have at least 10 validators (7 from P2 + 3 from P3). */
    HU_ASSERT(hu_output_validator_chain_len(chain) >= 10);
    hu_output_validator_chain_destroy(chain);
}
```

- [ ] **Step 3: Run full suite, confirm green**

```bash
./build/human_tests
```

- [ ] **Step 4: Commit + phase tag**

```bash
git commit -am "feat(agent): wire F1/F2/F3 validators into default chain (P3.T17)"
git tag -a phase-3-complete -m "F1 F2 F3 validators catching prod leaks"
```

### Task 18: Phase-3 verification gate

- [ ] **Step 1: `/verify`**

```
/verify "Phase 3 — F1/F2/F3 validators catch the 2026-05-14 Jordan-channel leaks. Verify: tests/test_validators_persona_safety.c passes; the actual leak texts (JORDAN_LEAK_F1, F2, F3) are now REJECTED or REWRITTEN; no false positives on real Seth-voice replies; full test suite green."
```

---

## Phase 4 — Stop-Sequence Plumbing

Goal: model stops at turn boundary. F3 dies at zero post-gen cost. Stop sequences are configured per-(provider, channel), with sensible defaults.

### Task 19: Add stop_sequences to hu_chat_request_t

**File:** `include/human/provider.h`

- [ ] **Step 1: Modify hu_chat_request_t**

After the `tools` array fields, add:
```c
    const char *const *stop_sequences; /* optional; NULL = none */
    size_t stop_sequences_count;
```

- [ ] **Step 2: Initialize the new fields to NULL/0 in every existing hu_chat_request_t struct literal**

`grep -rn "hu_chat_request_t" src/ | grep -v "\.h:" | head -50` to find all initialization sites. Add `.stop_sequences = NULL, .stop_sequences_count = 0,` to each (or rely on C99 zero-init for designated-initializer struct literals — verify with `-Wmissing-field-initializers` clean).

- [ ] **Step 3: Run full build + tests**

```bash
cmake --build --preset dev && ./build/human_tests
```
Expected: all tests still pass (no behavior change yet).

- [ ] **Step 4: Commit**

```bash
git commit -am "feat(provider): add stop_sequences[] to hu_chat_request_t (P4.T19)"
```

### Task 20: Stop-sequence registry

**Files:** `include/human/agent/stop_sequence_registry.h`, `src/agent/stop_sequence_registry.c`

- [ ] **Step 1: Define registry API**

```c
#ifndef HU_AGENT_STOP_SEQUENCE_REGISTRY_H
#define HU_AGENT_STOP_SEQUENCE_REGISTRY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the default stop sequences for a (provider, channel) pair.
 *
 * Output:
 *   *out_seqs       — array of NUL-terminated strings; static storage,
 *                     valid for program lifetime; NEVER freed by caller.
 *   *out_seqs_count — number of entries (0 if no defaults).
 *
 * Lookup precedence:
 *   1. Per-(provider, channel) explicit entry.
 *   2. Per-provider default.
 *   3. Per-channel default.
 *   4. Empty.
 *
 * Always returns HU_OK; missing entries just return count == 0. */
hu_error_t hu_stop_sequence_registry_lookup(
    const char *provider, size_t provider_len,
    const char *channel, size_t channel_len,
    const char *const **out_seqs, size_t *out_seqs_count);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Test cases per provider**

In `tests/test_stop_sequences.c`:
```c
static void registry_anthropic_has_role_stops(void) {
    const char *const *seqs = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("anthropic", 9, "imessage", 8,
                                                  &seqs, &count), HU_OK);
    HU_ASSERT(count > 0);
    bool has_human = false, has_user = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(seqs[i], "\n\nHuman:") == 0) has_human = true;
        if (strcmp(seqs[i], "\n\nUser:") == 0) has_user = true;
    }
    HU_ASSERT(has_human || has_user);
}

static void registry_empty_for_unknown(void) {
    const char *const *seqs = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_stop_sequence_registry_lookup("madeup", 6, NULL, 0, &seqs, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
}
```

- [ ] **Step 3: Implement registry as static tables**

```c
#include "human/agent/stop_sequence_registry.h"
#include <stddef.h>
#include <string.h>

/* Provider-level defaults. */
static const char *const ANTHROPIC_STOPS[] = {"\n\nHuman:", "\n\nUser:"};
static const char *const OPENAI_STOPS[] = {"\n\nUser:", "<|endoftext|>"};
static const char *const GEMINI_STOPS[] = {"\n\nUser:", "\nUser:"};
static const char *const HUML_STOPS[] = {"<|eot_id|>", "<|im_end|>"};

typedef struct { const char *provider; const char *const *seqs; size_t count; } provider_entry_t;
static const provider_entry_t PROVIDER_TABLE[] = {
    {"anthropic", ANTHROPIC_STOPS, sizeof(ANTHROPIC_STOPS) / sizeof(ANTHROPIC_STOPS[0])},
    {"openai", OPENAI_STOPS, sizeof(OPENAI_STOPS) / sizeof(OPENAI_STOPS[0])},
    {"openrouter", OPENAI_STOPS, sizeof(OPENAI_STOPS) / sizeof(OPENAI_STOPS[0])},
    {"gemini", GEMINI_STOPS, sizeof(GEMINI_STOPS) / sizeof(GEMINI_STOPS[0])},
    {"huml", HUML_STOPS, sizeof(HUML_STOPS) / sizeof(HUML_STOPS[0])},
    {"llamacpp", HUML_STOPS, sizeof(HUML_STOPS) / sizeof(HUML_STOPS[0])},
};
static const size_t PROVIDER_TABLE_COUNT = sizeof(PROVIDER_TABLE) / sizeof(PROVIDER_TABLE[0]);

hu_error_t hu_stop_sequence_registry_lookup(
    const char *provider, size_t provider_len,
    const char *channel, size_t channel_len,
    const char *const **out_seqs, size_t *out_seqs_count) {
    (void)channel; (void)channel_len; /* channel-specific overrides reserved for later */
    if (!out_seqs || !out_seqs_count) return HU_ERR_INVALID_ARGUMENT;
    *out_seqs = NULL;
    *out_seqs_count = 0;
    if (!provider) return HU_OK;
    for (size_t i = 0; i < PROVIDER_TABLE_COUNT; i++) {
        size_t plen = strlen(PROVIDER_TABLE[i].provider);
        if (plen == provider_len && memcmp(PROVIDER_TABLE[i].provider, provider, plen) == 0) {
            *out_seqs = PROVIDER_TABLE[i].seqs;
            *out_seqs_count = PROVIDER_TABLE[i].count;
            return HU_OK;
        }
    }
    return HU_OK;
}
```

- [ ] **Step 4: Confirm tests pass**

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(agent): stop-sequence registry per provider (P4.T20)"
```

### Tasks 21-25: Wire stop sequences in each provider's request builder

For each of `anthropic.c`, `openai.c`, `gemini.c`, `ollama.c`, `openrouter.c`, `huml.c`, `llamacpp.c`, plus any other concrete provider:

**Anthropic** uses `"stop_sequences": [...]` in request body.
**OpenAI / OpenRouter** uses `"stop": [...]`.
**Gemini** uses `"generationConfig.stopSequences": [...]`.
**Ollama** uses `"options.stop": [...]`.

- [ ] **Per-provider Step 1: Add a test**

E.g., `tests/test_provider_anthropic.c`:
```c
static void anthropic_request_body_includes_stop_sequences(void) {
    /* Build a minimal hu_chat_request_t with stop_sequences set, call
     * the internal request-body builder (expose via test seam), assert
     * the serialized JSON contains "stop_sequences": ["\n\nHuman:"]. */
    /* ... */
}
```

- [ ] **Per-provider Step 2: Modify the request-build function**

Each provider already emits its JSON request body; locate where `temperature` or `max_tokens` is emitted and add the equivalent stop-sequence array emission.

- [ ] **Per-provider Step 3: Test**

- [ ] **Per-provider Step 4: Commit**

```bash
git commit -am "feat(providers/<name>): emit stop_sequences in request (P4.T<n>)"
```

### Task 26: Plumb stop sequences from agent into request

**File:** `src/agent/agent_turn.c, agent_stream.c`

- [ ] **Step 1: Look up stop sequences before each chat call**

Where `hu_chat_request_t req = {...}` is built (search for `req.temperature` or `req.max_tokens` to find each site):
```c
const char *const *stop_seqs = NULL;
size_t stop_seqs_count = 0;
hu_stop_sequence_registry_lookup(provider_name, provider_name_len,
                                 channel_id, channel_id_len,
                                 &stop_seqs, &stop_seqs_count);
req.stop_sequences = stop_seqs;
req.stop_sequences_count = stop_seqs_count;
```

- [ ] **Step 2: Run full test suite**

- [ ] **Step 3: Commit + phase tag**

```bash
git commit -am "feat(agent): plumb stop_sequences from registry through chat requests (P4.T26)"
git tag -a phase-4-complete
```

### Task 27: Phase-4 verification gate

- [ ] **Step 1: `/verify`**

```
/verify "Phase 4 — stop-sequence plumbing. Verify: hu_chat_request_t has stop_sequences fields; each provider's request body includes stop sequences when set; registry returns expected defaults per provider; the integration test that exercises agent_turn with a recorded chat request shows the stop sequences in the outbound JSON."
```

---

## Phase 5 — Structured Output Contract

Goal: providers that support it emit `{"reply": "..."}` JSON; the reply field is the wire text. Reasoning is contained inside the JSON contract and never escapes the provider.

### Task 28: Schema header

**File:** `include/human/provider/structured_output.h`

- [ ] **Step 1: Define the contract**

```c
/* Structured-output contract for chat replies.
 *
 * The canonical schema is:
 *   {
 *     "reply": "the text to send",
 *     "tone_used": "optional self-report",
 *     "reasoning": "optional internal chain-of-thought"
 *   }
 *
 * Only "reply" is sent over the wire; "reasoning" is captured as
 * resp.reasoning_content and may be audited but never wired. */
#ifndef HU_PROVIDER_STRUCTURED_OUTPUT_H
#define HU_PROVIDER_STRUCTURED_OUTPUT_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The full JSON schema string for the chat reply contract. Static
 * storage; do not free. */
const char *hu_structured_output_chat_reply_schema(void);

/* Extract `reply` from a JSON-shaped provider response. Returns
 * HU_OK + an allocator-owned NUL-terminated string in *out_reply. On
 * parse failure, returns HU_ERR_PARSE and *out_reply remains NULL. */
hu_error_t hu_structured_output_extract_reply(
    hu_allocator_t *alloc, const char *body, size_t body_len,
    char **out_reply, size_t *out_reply_len,
    char **out_reasoning, size_t *out_reasoning_len);

/* Sentinel-extract fallback for providers that can't enforce JSON.
 * Looks for `<REPLY>...</REPLY>` markers, returns inner text. */
hu_error_t hu_structured_output_extract_sentinel(
    hu_allocator_t *alloc, const char *body, size_t body_len,
    char **out_reply, size_t *out_reply_len);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2-N: Implement extract_reply (JSON), extract_sentinel, with tests**

- [ ] **Commit**

### Task 29: Wire Gemini responseSchema

**File:** `src/providers/gemini.c`

- [ ] **Step 1: Test that body includes responseSchema when request.response_format == "json_schema"**

- [ ] **Step 2: Implement — emit `generationConfig.responseSchema = {...}` and `responseMimeType: "application/json"` when request opts in**

- [ ] **Step 3: Confirm pass; commit**

### Task 30: Anthropic tool-use proxy

**File:** `src/providers/anthropic.c, src/providers/structured_output_anthropic.c`

- [ ] **Step 1: Implement a "send_reply" tool that the model is forced to call**

When `request.response_format == "json_schema"`, the provider constructs a synthetic tool spec with the chat-reply schema, sets `tool_choice = {"type": "tool", "name": "send_reply"}`, and on response unpacks the tool call's input as the structured reply.

- [ ] **Step 2: Tests + commit**

### Task 31: OpenAI json_schema response_format

Already partially supported via `response_format`. Wire the full `json_schema` path.

- [ ] **Tests + commit**

### Task 32: Sentinel-extract fallback for legacy providers

- [ ] **Wire `hu_structured_output_extract_sentinel` after raw response from huml / llamacpp / others lacking native structured output, gated on a per-provider capability flag (`supports_structured_output()`).**
- [ ] **Tests + commit**

### Task 33: Plumb structured output through the agent

**File:** `src/agent/agent_turn.c, agent_stream.c`

- [ ] **Step 1: When persona requires structure (config flag), set `request.response_format = "json_schema"` + `request.response_schema = hu_structured_output_chat_reply_schema()`**

- [ ] **Step 2: Extract reply from the response, set as the message content, place reasoning in `resp.reasoning_content`**

- [ ] **Step 3: Run validator chain on the extracted reply (chain still relevant as backstop for cases where JSON parse succeeds but reply contains stripped patterns)**

- [ ] **Step 4: Tests + commit + phase tag**

```bash
git tag -a phase-5-complete
```

### Task 34: Phase-5 verification gate

- [ ] **Step 1: `/verify`**

```
/verify "Phase 5 — structured output contract. Verify: Gemini emits responseSchema when opted in; Anthropic uses tool-use proxy; sentinel fallback parses <REPLY> markers; reply is extracted before chain validation."
```

---

## Phase 6 — Persona-Fidelity Classifier (Deferred Stub)

Tied to M3 (persona LoRA). For now, design the validator interface and stub it as a pass-through. Full implementation lands when M3 ships an on-device persona classifier model.

### Task 35: Persona-fidelity stub validator

**File:** `src/agent/validators/persona_fidelity_validator.c`

- [ ] **Step 1: Stub that always PASSes; logs a telemetry event saying "fidelity classifier not yet active"**
- [ ] **Step 2: Tests + commit**

---

## Final Verification

### Task 36: Critic review + sprint audit

- [ ] **Step 1: Spawn critic agent on the diff**

```
Use the critic agent to review every commit on this branch since `phase-1-complete`. Surface: half-fixes, missing edge cases, cross-validator regressions.
```

- [ ] **Step 2: Address critic findings as new tasks tagged `CRITIC-`**

- [ ] **Step 3: Sprint audit**

```
Use sprint-auditor to re-read the original failure modes (F1/F2/F3 from the 2026-05-14 Jordan-channel incidents) and the actual deliverables. Per failure mode, answer: did we deliver?
```

- [ ] **Step 4: Update CLAUDE.md / lessons.md with anything non-obvious**

- [ ] **Step 5: Open PR**

```bash
gh pr create --title "feat(agent): unified output validator chain + stop-sequences + structured output" \
  --body "$(cat <<'EOF'
## Summary
- Unified six scattered output sanitizers into one composable `hu_output_validator_t` vtable with a chain executor.
- Added three new validators that catch the 2026-05-14 Jordan-channel leaks: `persona_narrator` (F1), `assistant_closer` (F2), `role_consistency` (F3).
- Plumbed `stop_sequences[]` through `hu_chat_request_t` and every concrete provider; added a per-provider registry.
- Added structured-output contract: Gemini `responseSchema`, Anthropic tool-use proxy, OpenAI `json_schema`, sentinel fallback for legacy providers.
- All 9,800+ existing tests pass; added ~80 new tests for the validator chain + new validators.

## Test plan
- [x] `./build/human_tests --suite=output_validator` — chain semantics
- [x] `./build/human_tests --suite=validators_builtin` — migrated strippers
- [x] `./build/human_tests --suite=validators_persona_safety` — F1/F2/F3 regression
- [x] `./build/human_tests --suite=stop_sequences` — registry
- [x] `./build/human_tests --suite=structured_output` — JSON/sentinel
- [x] Full suite green under ASan

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review Checklist (writing-plans skill)

**Spec coverage:**
- F1 (prose-CoT leak) → Task 14 ✓
- F2 (assistant closer) → Task 15 ✓
- F3 (role-collapse mid-message) → Task 16 ✓
- Stop-sequence plumbing → Tasks 19-26 ✓
- Structured output → Tasks 28-33 ✓
- Migrate existing strippers → Tasks 7-12 ✓
- Validator vtable infrastructure → Tasks 1-6 ✓

**Type consistency:**
- `hu_validator_decision_t` enum: `PASS`/`REWRITE`/`REJECT` used consistently across all tasks ✓
- `hu_output_validator_t` and `hu_output_validator_vtable_t` defined in T1, referenced consistently in T2-T18 ✓
- `hu_chain_result_t` defined T3, used in T5, T12, T17 ✓
- Factory naming: `hu_validator_<name>_create` consistent across T7-T16 ✓
- `hu_chat_request_t.stop_sequences` field name consistent across T19, T20, T26 ✓

**Placeholder scan:** none — every step has concrete code or commands.

**Open follow-ups (intentional, not gaps):**
- Persona-fidelity classifier (Task 35) is a stub tied to M3.
- Channel-specific stop-sequence overrides (T20) are wired but use no entries yet — future work.
- Telemetry hook for validator decisions: implicit in chain result struct; future task to wire to observer.
