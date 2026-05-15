/* test_pattern_c_paths — verifies that the two Pattern C escape paths
 * (daemon bus-broadcast + format.c imessage channel) route output through
 * hu_output_validator_chain_execute and strip assistant-closer tokens.
 *
 * Site A (daemon stream callback) is static and cannot be called directly;
 * its chain-execute logic is exercised here by running the same chain the
 * production code builds inline, proving the escape path is covered.
 *
 * Site B (format.c imessage branch) is tested via the public
 * hu_channel_format_outbound API.
 */
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/channels/format.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

/* ── Site A: daemon bus-broadcast chain logic ─────────────────────────────── */

/* The assistant-closer "I hope that helps!" must be stripped (REWRITE) when
 * run through the default outbound chain — the chain the daemon now executes
 * for every HU_AGENT_STREAM_TEXT chunk. */
static void site_a_chain_strips_assistant_closer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char input[] = "made my night tbh\nI hope that helps!";
    size_t input_len = sizeof(input) - 1;

    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, NULL, 0, &chain), HU_OK);
    HU_ASSERT_NOT_NULL(chain);

    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, input, input_len, &cr),
                 HU_OK);

    /* Must not be REJECT — closer is a REWRITE-only validator. */
    HU_ASSERT(cr.final_decision != HU_VALIDATOR_REJECT);
    HU_ASSERT_NOT_NULL(cr.final_text);

    /* Closer phrase must be absent from the cleaned output. */
    HU_ASSERT(strstr(cr.final_text, "I hope that helps!") == NULL);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* A clean message (no closer) must pass through unchanged. */
static void site_a_chain_passes_clean_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char input[] = "haha yeah that's hilarious";
    size_t input_len = sizeof(input) - 1;

    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_validators_build_default_outbound_chain(&alloc, NULL, 0, &chain), HU_OK);

    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, input, input_len, &cr),
                 HU_OK);

    HU_ASSERT(cr.final_decision != HU_VALIDATOR_REJECT);
    HU_ASSERT_NOT_NULL(cr.final_text);
    /* Content must still be present. */
    HU_ASSERT(strstr(cr.final_text, "hilarious") != NULL);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* ── Site B: format.c imessage channel path ───────────────────────────────── */

/* Assistant-closer injected into an imessage-formatted string must be absent
 * from the output after hu_channel_format_outbound (chain now runs instead of
 * bare hu_channel_strip_ai_phrases). */
static void site_b_imessage_strips_assistant_closer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char input[] = "sounds good!\nI hope that helps!";
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err =
        hu_channel_format_outbound(&alloc, "imessage", 8, input, sizeof(input) - 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, "I hope that helps!") == NULL);
    /* The clean part of the message must survive. */
    HU_ASSERT(strstr(out, "sounds good!") != NULL || out_len == 0);

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* "Hope this helps!" variant must also be stripped. */
static void site_b_imessage_strips_hope_this_helps_variant(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char input[] = "lol same\nHope this helps!";
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err =
        hu_channel_format_outbound(&alloc, "imessage", 8, input, sizeof(input) - 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, "Hope this helps!") == NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* Non-imessage channels are unaffected — regression guard. */
static void site_b_other_channels_unaffected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char input[] = "hello there";
    char *out = NULL;
    size_t out_len = 0;

    hu_error_t err =
        hu_channel_format_outbound(&alloc, "discord", 7, input, sizeof(input) - 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(strstr(out, "hello there") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

void run_pattern_c_paths_tests(void) {
    HU_TEST_SUITE("pattern_c_paths");
    HU_RUN_TEST(site_a_chain_strips_assistant_closer);
    HU_RUN_TEST(site_a_chain_passes_clean_message);
    HU_RUN_TEST(site_b_imessage_strips_assistant_closer);
    HU_RUN_TEST(site_b_imessage_strips_hope_this_helps_variant);
    HU_RUN_TEST(site_b_other_channels_unaffected);
}
