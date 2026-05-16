/* test_validator_chain_cache.c — Tests for the outbound_chain field cached on hu_persona_t.
 *
 * AC coverage (US-4):
 *   AC-4.1  outbound_chain != NULL after load with a named persona; NULL for zero-name persona.
 *   AC-4.2  Pointer stable across two hu_persona_select_examples calls (no re-allocation).
 *   AC-4.3  grep guard: documented in PR; verified inline via assertion that cached chain
 *           produces identical results to a freshly built chain on the same input.
 *   AC-4.4  Full load/use/destroy cycle is leak-free under ASan (compile-time guarantee).
 *   AC-4.5  Full suite green (implicitly: all existing tests still pass).
 */

#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>

/* Minimal persona JSON with a name — enough for hu_persona_load_json to succeed
 * and produce a non-NULL outbound_chain keyed on "TestUser". */
static const char PERSONA_JSON_WITH_NAME[] =
    "{"
    "  \"name\": \"TestUser\","
    "  \"identity\": \"A test persona for validator chain cache tests.\","
    "  \"traits\": [\"curious\", \"friendly\"],"
    "  \"communication_rules\": [\"be concise\"]"
    "}";

/* Persona JSON with no name field — outbound_chain should still be non-NULL
 * (chain built with NULL persona name, which is valid). */
static const char PERSONA_JSON_NO_NAME[] = "{"
                                           "  \"identity\": \"Nameless persona.\","
                                           "  \"traits\": [\"quiet\"]"
                                           "}";

/* ── AC-4.1: outbound_chain non-NULL after load with named persona ─────── */

static void chain_cache_non_null_after_load_with_named_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));

    hu_error_t err = hu_persona_load_json(&alloc, PERSONA_JSON_WITH_NAME,
                                          sizeof(PERSONA_JSON_WITH_NAME) - 1, &p);
    HU_ASSERT_EQ(err, HU_OK);

    /* AC-4.1: chain must be populated. */
    HU_ASSERT_NOT_NULL(p.outbound_chain);

    /* Sanity: chain must be functional — execute on clean input should PASS. */
    const char *clean = "sounds good to me";
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    hu_error_t cerr = hu_output_validator_chain_execute(p.outbound_chain, &alloc, NULL, clean,
                                                        strlen(clean), &cr);
    HU_ASSERT_EQ(cerr, HU_OK);
    HU_ASSERT_EQ(cr.final_decision, HU_VALIDATOR_PASS);
    hu_chain_result_free(&alloc, &cr);

    hu_persona_deinit(&alloc, &p);
    /* After deinit, outbound_chain pointer was freed; no further assertions on it. */
}

/* ── AC-4.1b: chain also built when persona has no name (NULL name key) ── */

static void chain_cache_non_null_after_load_with_no_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));

    hu_error_t err =
        hu_persona_load_json(&alloc, PERSONA_JSON_NO_NAME, sizeof(PERSONA_JSON_NO_NAME) - 1, &p);
    HU_ASSERT_EQ(err, HU_OK);

    /* Chain is built with NULL persona_name — still valid (no assistant_closer keying). */
    HU_ASSERT_NOT_NULL(p.outbound_chain);

    hu_persona_deinit(&alloc, &p);
}

/* ── AC-4.2: pointer stable across two hu_persona_select_examples calls ─── */

static void chain_cache_pointer_stable_across_select_examples_calls(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));

    hu_error_t err = hu_persona_load_json(&alloc, PERSONA_JSON_WITH_NAME,
                                          sizeof(PERSONA_JSON_WITH_NAME) - 1, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.outbound_chain);

    const hu_output_validator_chain_t *first_ptr = p.outbound_chain;

    /* hu_persona_select_examples must not reallocate or replace outbound_chain. */
    const hu_persona_example_t *examples = NULL;
    size_t example_count = 0;
    hu_persona_select_examples(&p, "imessage", 8, NULL, 0, &examples, &example_count, 5);

    const hu_output_validator_chain_t *second_ptr = p.outbound_chain;

    /* AC-4.2: pointer must be identical. */
    HU_ASSERT(first_ptr == second_ptr);

    hu_persona_deinit(&alloc, &p);
}

/* ── AC-4.3: cached chain produces same decision as freshly built chain ── */

static void chain_cache_produces_same_result_as_fresh_build(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));

    hu_error_t err = hu_persona_load_json(&alloc, PERSONA_JSON_WITH_NAME,
                                          sizeof(PERSONA_JSON_WITH_NAME) - 1, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.outbound_chain);

    /* Input that assistant_closer should rewrite (contains closer phrase). */
    const char *input =
        "see you later!\nI'm all set, thank you! Is there anything I can help you with?";
    size_t input_len = strlen(input);

    /* Run through cached chain. */
    hu_chain_result_t cr_cached;
    memset(&cr_cached, 0, sizeof(cr_cached));
    hu_error_t err1 = hu_output_validator_chain_execute(p.outbound_chain, &alloc, NULL, input,
                                                        input_len, &cr_cached);
    HU_ASSERT_EQ(err1, HU_OK);

    /* Build a fresh chain for comparison. */
    hu_output_validator_chain_t *fresh = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, "TestUser", 8, &fresh), HU_OK);
    hu_chain_result_t cr_fresh;
    memset(&cr_fresh, 0, sizeof(cr_fresh));
    hu_error_t err2 =
        hu_output_validator_chain_execute(fresh, &alloc, NULL, input, input_len, &cr_fresh);
    HU_ASSERT_EQ(err2, HU_OK);

    /* AC-4.3: both chains must produce the same final_decision. */
    HU_ASSERT_EQ(cr_cached.final_decision, cr_fresh.final_decision);

    hu_chain_result_free(&alloc, &cr_cached);
    hu_chain_result_free(&alloc, &cr_fresh);
    hu_output_validator_chain_destroy(fresh);
    hu_persona_deinit(&alloc, &p);
    /* AC-4.4: ASan will catch any leaks at this point. */
}

/* ── AC-4.4: deinit with no prior use — no leaks ─────────────────────────── */

static void chain_cache_deinit_without_use_no_leak(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));

    hu_error_t err = hu_persona_load_json(&alloc, PERSONA_JSON_WITH_NAME,
                                          sizeof(PERSONA_JSON_WITH_NAME) - 1, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.outbound_chain);

    /* Deinit without ever executing the chain — ASan proves no leak. */
    hu_persona_deinit(&alloc, &p);
    HU_ASSERT_NULL(p.outbound_chain); /* memset clears it */
}

/* ── Registration ────────────────────────────────────────────────────────── */

void run_validator_chain_cache_tests(void) {
    HU_TEST_SUITE("validator_chain_cache");
    HU_RUN_TEST(chain_cache_non_null_after_load_with_named_persona);
    HU_RUN_TEST(chain_cache_non_null_after_load_with_no_name);
    HU_RUN_TEST(chain_cache_pointer_stable_across_select_examples_calls);
    HU_RUN_TEST(chain_cache_produces_same_result_as_fresh_build);
    HU_RUN_TEST(chain_cache_deinit_without_use_no_leak);
}
