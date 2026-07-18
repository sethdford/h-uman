/* test_daemon_shape.c — pins the reactive send-path text shaper.
 *
 * The whole point of Phase 1 (2026-07-12 egress audit): the humanness mutators
 * ran inline in daemon.c under `#ifndef HU_IS_TEST`, so the suite never
 * exercised the production shaping ORDER. hu_daemon_shape_text_inplace lifts
 * the pure, order-dependent stages into one unit that IS compiled into tests.
 *
 * The core test is a characterization test: the orchestrator's output must
 * byte-equal the same five public functions called in sequence with the same
 * seed. That is the "the move changed nothing" contract — no hardcoded golden
 * string to drift, and it fails against a no-op stub.
 */

#include "human/agent/style_governor.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/daemon/daemon_shape.h"
#include "human/persona.h"
#include "test_framework.h"

#include <string.h>

/* Grow *buf to hold at least len+16 bytes, mirroring the daemon's original
 * inline grow logic (realloc failure leaves buffer + capacity unchanged). */
static void ref_grow(hu_allocator_t *a, char **buf, size_t len, size_t *cap) {
    if (*cap < len + 16) {
        char *g = (char *)a->realloc(a->ctx, *buf, *cap, len + 16);
        if (g) {
            *buf = g;
            *cap = len + 16;
        }
    }
}

/* The pre-extraction sequence, expressed via the individual public functions.
 * This is the ground-truth "pre-refactor behavior" the orchestrator must
 * reproduce byte-for-byte. */
static void reference_shape(hu_allocator_t *a, char **buf, size_t *len, size_t *cap, uint32_t seed,
                            const hu_persona_overlay_t *overlay,
                            const hu_contact_profile_t *contact, const char *formality,
                            size_t formality_len, const char *ch, size_t ch_len, float freq) {
    if (overlay && overlay->typing_quirks && overlay->typing_quirks_count > 0)
        *len = hu_conversation_apply_typing_quirks(
            *buf, *len, (const char *const *)overlay->typing_quirks, overlay->typing_quirks_count);
    *len = hu_conversation_vary_complexity(*buf, *len, seed);
    ref_grow(a, buf, *len, cap);
    *len = hu_conversation_apply_fillers(*buf, *len, *cap, seed, ch, ch_len);
    ref_grow(a, buf, *len, cap);
    *len = hu_conversation_apply_disfluency(*buf, *len, *cap, seed, freq, contact, formality,
                                            formality_len);
    *len = hu_style_governor_apply_inplace(a, *buf, *len);
}

/* Allocate a fresh buf holding `s` with capacity exactly len+1 (matching the
 * daemon's initial response_alloc_len = response_len convention). */
static char *fresh_buf(hu_allocator_t *a, const char *s, size_t *len, size_t *cap) {
    *len = strlen(s);
    *cap = *len + 1;
    char *b = (char *)a->alloc(a->ctx, *cap);
    memcpy(b, s, *cap);
    return b;
}

/* Core contract: orchestrator output == reference sequence, byte-for-byte.
 * Governor + disfluency forced LIVE so at least one stage definitely mutates,
 * which distinguishes the real orchestrator from a no-op stub. */
static void shape_composition_matches_reference_sequence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_LIVE);
    hu_conversation_disfluency_set_mode_for_test(HU_DISFLUENCY_LIVE);

    static const char *inputs[] = {
        "Sounds good.",
        "yeah I will be there in a bit",
        "It is fine, see you then.",
        "haha that is wild, I cannot believe it happened",
        "ok",
        "Wish I could, save a spot for me next time",
    };
    const uint32_t seeds[] = {1u, 12345u, 0xDEADBEEFu, 777u};
    int any_mutated = 0;

    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
            size_t rlen, rcap, olen, ocap;
            char *ref = fresh_buf(&alloc, inputs[i], &rlen, &rcap);
            char *orc = fresh_buf(&alloc, inputs[i], &olen, &ocap);

            reference_shape(&alloc, &ref, &rlen, &rcap, seeds[s], NULL, NULL, NULL, 0, "imessage",
                            8, 1.0f);
            hu_daemon_shape_text_inplace(&alloc, &orc, &olen, &ocap, seeds[s], NULL, NULL, NULL, 0,
                                         "imessage", 8, 1.0f);

            HU_ASSERT_EQ(olen, rlen);
            HU_ASSERT_STR_EQ(orc, ref);
            if (strcmp(inputs[i], orc) != 0)
                any_mutated = 1;

            alloc.free(alloc.ctx, ref, rcap);
            alloc.free(alloc.ctx, orc, ocap);
        }
    }
    /* Guards the test itself: if nothing ever mutated, the comparison above
     * would pass vacuously against a stub. */
    HU_ASSERT_TRUE(any_mutated);

    hu_style_governor_set_mode_for_test(-1);
    hu_conversation_disfluency_set_mode_for_test(-1);
}

/* At default gates the composition is fully deterministic given the seed, and
 * equals the reference sequence (the exact composed output — no golden string
 * to rot). This is the shape the disfluency incident would have tripped. */
static void shape_all_default_gates_is_deterministic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_OFF);
    hu_conversation_disfluency_set_mode_for_test(HU_DISFLUENCY_OFF);

    const char *input = "Sounds good.";
    const uint32_t seed = 424242u;

    size_t alen, acap, blen, bcap, rlen, rcap;
    char *a = fresh_buf(&alloc, input, &alen, &acap);
    char *b = fresh_buf(&alloc, input, &blen, &bcap);
    char *ref = fresh_buf(&alloc, input, &rlen, &rcap);

    hu_daemon_shape_text_inplace(&alloc, &a, &alen, &acap, seed, NULL, NULL, NULL, 0, "imessage", 8,
                                 0.15f);
    hu_daemon_shape_text_inplace(&alloc, &b, &blen, &bcap, seed, NULL, NULL, NULL, 0, "imessage", 8,
                                 0.15f);
    reference_shape(&alloc, &ref, &rlen, &rcap, seed, NULL, NULL, NULL, 0, "imessage", 8, 0.15f);

    /* Same seed → identical output (determinism). */
    HU_ASSERT_EQ(alen, blen);
    HU_ASSERT_STR_EQ(a, b);
    /* And that output IS the exact composed pipeline result. */
    HU_ASSERT_EQ(alen, rlen);
    HU_ASSERT_STR_EQ(a, ref);

    alloc.free(alloc.ctx, a, acap);
    alloc.free(alloc.ctx, b, bcap);
    alloc.free(alloc.ctx, ref, rcap);

    hu_style_governor_set_mode_for_test(-1);
    hu_conversation_disfluency_set_mode_for_test(-1);
}

/* The 2026-07-12 incident: disfluency inserted " wait no " mid-sentence into a
 * clean reply to a real contact. Disfluency now defaults OFF; the orchestrator
 * must never inject it (or any disfluency marker) at the default gate — even on
 * text that would have triggered it. */
static void shape_disfluency_stays_off_by_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_OFF);
    hu_conversation_disfluency_set_mode_for_test(HU_DISFLUENCY_OFF);

    static const char *inputs[] = {
        "Wish I could, save a spot for me next time",
        "yeah I mean it depends on the day honestly",
        "that sounds like a good plan to me",
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        for (uint32_t seed = 0; seed < 32; seed++) {
            size_t len, cap;
            char *buf = fresh_buf(&alloc, inputs[i], &len, &cap);
            hu_daemon_shape_text_inplace(&alloc, &buf, &len, &cap, seed, NULL, NULL, NULL, 0,
                                         "imessage", 8, 1.0f);
            HU_ASSERT_TRUE(strstr(buf, " wait no ") == NULL);
            HU_ASSERT_TRUE(strstr(buf, "*meant it") == NULL);
            alloc.free(alloc.ctx, buf, cap);
        }
    }

    hu_style_governor_set_mode_for_test(-1);
    hu_conversation_disfluency_set_mode_for_test(-1);
}

/* Buffer-ownership stress: many iterations across seeds, inputs, and LIVE gates
 * (which force the fillers/disfluency realloc-grow paths). ASan proves no leak,
 * no overflow, and that *len always matches the NUL-terminated content. */
static void shape_preserves_buffer_ownership(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_governor_set_mode_for_test(HU_STYLE_GOVERNOR_LIVE);
    hu_conversation_disfluency_set_mode_for_test(HU_DISFLUENCY_LIVE);

    static const char *inputs[] = {
        "",
        "ok",
        "Sounds good.",
        "yeah I will be there in a bit, cannot wait to see everyone",
        "It is a really long message that keeps going and going and going so that the filler and "
        "disfluency injectors have plenty of room to insert things and force the buffer to grow "
        "more than once during the composition",
    };
    const size_t n_inputs = sizeof(inputs) / sizeof(inputs[0]);

    for (uint32_t seed = 0; seed < 500; seed++) {
        const char *in = inputs[seed % n_inputs];
        size_t len, cap;
        char *buf = fresh_buf(&alloc, in, &len, &cap);
        hu_daemon_shape_text_inplace(&alloc, &buf, &len, &cap, seed, NULL, NULL, NULL, 0,
                                     "imessage", 8, 1.0f);
        /* Invariants: NUL-terminated, *len == strlen, capacity holds it. */
        HU_ASSERT_EQ(len, strlen(buf));
        HU_ASSERT_TRUE(cap >= len + 1);
        alloc.free(alloc.ctx, buf, cap);
    }

    hu_style_governor_set_mode_for_test(-1);
    hu_conversation_disfluency_set_mode_for_test(-1);
}

/* Degenerate inputs must be safe no-ops. */
static void shape_handles_empty_and_zero_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0, cap = 1;
    char *buf = (char *)alloc.alloc(alloc.ctx, cap);
    buf[0] = '\0';
    hu_daemon_shape_text_inplace(&alloc, &buf, &len, &cap, 7u, NULL, NULL, NULL, 0, "imessage", 8,
                                 0.15f);
    HU_ASSERT_EQ(len, 0u);
    HU_ASSERT_EQ(buf[0], '\0');
    alloc.free(alloc.ctx, buf, cap);
}

void run_daemon_shape_tests(void) {
    HU_TEST_SUITE("daemon_shape");
    HU_RUN_TEST(shape_composition_matches_reference_sequence);
    HU_RUN_TEST(shape_all_default_gates_is_deterministic);
    HU_RUN_TEST(shape_disfluency_stays_off_by_default);
    HU_RUN_TEST(shape_preserves_buffer_ownership);
    HU_RUN_TEST(shape_handles_empty_and_zero_len);
}
