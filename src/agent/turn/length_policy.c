/* Reply-length policy. Contract: include/human/agent/length_policy.h. */
#include "human/agent/length_policy.h"
#include "human/context/conversation.h"
#include "human/core/log.h"
#include "human/persona.h"
#include <stdlib.h>
#include <string.h>

bool hu_length_policy_legacy_tight(uint32_t cap) {
    return cap > 0 && cap <= HU_LENGTH_POLICY_LEGACY_TIGHT_MAX;
}

hu_length_policy_result_t hu_length_policy_compute(const hu_length_policy_input_t *in) {
    hu_length_policy_result_t r = {0};
    if (!in)
        return r;
    r.cap = in->legacy_cap;
    if (in->contact_p50 == 0 && in->contact_p90 == 0) {
        r.tight = hu_length_policy_legacy_tight(in->legacy_cap);
        return r;
    }
    r.from_stats = true;
    uint32_t p50 = in->contact_p50;
    if (p50 == 0) {
        p50 = in->contact_p90 * HU_LENGTH_POLICY_P50_NUM / HU_LENGTH_POLICY_P50_DEN;
        if (p50 == 0)
            p50 = 1;
        r.p50_derived = true;
    }
    r.p50 = p50;

    /* Today's cap is the starting point and is never lowered: it already
     * scales with the inbound (per-contact multipliers included), keeps the
     * 15-char floor and is floored at the owner's p90. A question or a story
     * opens up further: brief mode no longer caps it. */
    uint32_t bound = HU_LENGTH_POLICY_HARD_MAX;
    if (in->hard_max > 0 && in->hard_max < bound)
        bound = in->hard_max;
    bool opens = (in->shape & (HU_LENGTH_SHAPE_QUESTION | HU_LENGTH_SHAPE_STORY)) != 0;
    uint32_t open_cap = in->unbriefed_cap < bound ? in->unbriefed_cap : bound;
    if (opens && open_cap > r.cap)
        r.cap = open_cap;
    uint32_t floor = p50 < bound ? p50 : bound; /* never below the owner's own median */
    if (r.cap < floor)
        r.cap = floor;

    /* "Keep it tight" now describes the inbound, not the cap: a short, casual
     * message (no question, no story) shorter than the owner's own median. */
    r.tight = opens == false && in->inbound_len < p50;
    return r;
}

unsigned hu_length_policy_inbound_shape(const char *inbound, size_t len) {
    if (!inbound || len == 0)
        return 0;
    unsigned shape = 0;
    size_t breaks = 0;
    for (size_t i = 0; i < len; i++) {
        char c = inbound[i];
        if (c == '?')
            shape |= HU_LENGTH_SHAPE_QUESTION;
        if (c == '\n' ||
            ((c == '.' || c == '!' || c == '?') && i + 1 < len && inbound[i + 1] == ' '))
            breaks++;
    }
    /* Story: long, or a multi-sentence / multi-line message of some length. */
    if (len >= HU_LENGTH_POLICY_LONG_INBOUND || (len >= 80 && breaks >= 2))
        shape |= HU_LENGTH_SHAPE_STORY;
    return shape;
}

static uint32_t legacy_turn_cap(const hu_length_turn_t *t, uint32_t *unbriefed) {
    /* Moved verbatim from daemon.c (F15 ratio calibration, then brief mode). */
    uint32_t cap = t->channel_max;
    int cal = t->is_group ? hu_conversation_max_response_chars(t->inbound_len)
                          : hu_conversation_max_response_chars_relational(t->inbound_len,
                                                                          t->contact, t->stage);
    if (cal > 0 && (cap == 0 || (uint32_t)cal < cap))
        cap = (uint32_t)cal;
    *unbriefed = cap;
    if (t->brief_mode) {
        uint32_t bc = hu_conversation_brief_char_cap(t->is_group, t->contact, t->stage);
        if (cap > bc)
            cap = bc;
    }
    return cap;
}

void hu_length_policy_turn(const hu_length_turn_t *t, hu_gate_mode_t mode,
                           hu_length_turn_result_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!t)
        return;
    uint32_t unbriefed = 0;
    uint32_t old_cap = legacy_turn_cap(t, &unbriefed);
    out->cap = out->old_cap = out->new_cap = old_cap;
    out->tight = HU_LENGTH_TIGHT_LEGACY;
    if (mode == HU_GATE_OFF || t->is_group)
        return;

    uint32_t bound = hu_conversation_max_response_chars_ceiling();
    if (t->channel_max > 0 && t->channel_max < bound)
        bound = t->channel_max;
    unsigned shape = hu_length_policy_inbound_shape(t->inbound, t->inbound_len);
    hu_length_policy_input_t in = {
        .inbound_len = t->inbound_len,
        .shape = shape,
        .contact_p50 = t->contact ? t->contact->reply_chars_p50 : 0u,
        .contact_p90 = t->contact ? t->contact->reply_chars_p90 : 0u,
        .legacy_cap = old_cap,
        .unbriefed_cap = unbriefed,
        .hard_max = bound,
    };
    hu_length_policy_result_t r = hu_length_policy_compute(&in);
    out->new_cap = r.cap;
    out->from_stats = r.from_stats;
    bool tight_old = hu_length_policy_legacy_tight(old_cap);
    hu_log_info("length-policy", NULL,
                "[HU_LENGTH_POLICY %s] old_cap=%u new_cap=%u tight_old=%d tight_new=%d stats=%d "
                "p50=%u p50_derived=%d shape=%u inbound_len=%zu brief=%d stage=%d",
                mode == HU_GATE_LIVE ? "live" : "shadow", (unsigned)old_cap, (unsigned)r.cap,
                (int)tight_old, (int)r.tight, (int)r.from_stats, (unsigned)r.p50,
                (int)r.p50_derived, shape, t->inbound_len, (int)t->brief_mode, (int)t->stage);
    if (mode != HU_GATE_LIVE || !r.from_stats)
        return;
    out->cap = r.cap;
    out->tight = r.tight ? HU_LENGTH_TIGHT_YES : HU_LENGTH_TIGHT_NO;
}

size_t hu_length_policy_quality_over_ref(size_t ref_len, uint32_t max_chars, hu_gate_mode_t mode,
                                         bool cap_from_stats) {
    if (mode != HU_GATE_LIVE || !cap_from_stats || max_chars == 0)
        return ref_len;
    /* ratio <= 1.5 is full brevity marks; a reply of exactly max_chars lands there. */
    size_t cap_ref = ((size_t)max_chars * 2u + 2u) / 3u;
    return cap_ref > ref_len ? cap_ref : ref_len;
}

static int s_mode_override = -1;

hu_gate_mode_t hu_length_policy_mode(void) {
    if (s_mode_override >= 0)
        return (hu_gate_mode_t)s_mode_override;
    return hu_gate_mode_from_env("HU_LENGTH_POLICY", HU_GATE_OFF);
}

void hu_length_policy_set_mode_for_test(int mode) {
    s_mode_override = mode;
}
