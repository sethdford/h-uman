/* daemon_reactive_calibration.c — step 2c of the reactive reply, moved verbatim
 * from daemon.c (2026-10-02) so the replay harness shares it.
 * Contract: include/human/daemon/reactive_calibration.h. */
#include "human/daemon/reactive_calibration.h"

#include "human/agent.h"
#include "human/channel.h"
#include "human/context/conversation.h"
#include "human/persona.h"
#include <string.h>

void hu_daemon_append_length_calibration(hu_allocator_t *alloc, hu_agent_t *agent, const char *key,
                                         size_t key_len, const char *combined, size_t combined_len,
                                         bool is_group, char **convo_ctx, size_t *convo_ctx_len) {
    if (!alloc || !agent || !convo_ctx || !convo_ctx_len || !combined || combined_len == 0)
        return;
    char cal_buf[1024];
    const hu_contact_profile_t *cp_cal = (agent->persona && key && key_len > 0)
                                             ? hu_persona_find_contact(agent->persona, key, key_len)
                                             : NULL;
    size_t cal_len = hu_conversation_calibrate_length_for_contact(
        combined, combined_len, NULL, 0, is_group, cp_cal, agent->relationship.stage, cal_buf,
        sizeof(cal_buf));
    if (cal_len == 0)
        return;
    if (!*convo_ctx) {
        char *fresh = (char *)alloc->alloc(alloc->ctx, cal_len + 1);
        if (fresh) {
            memcpy(fresh, cal_buf, cal_len);
            fresh[cal_len] = '\0';
            *convo_ctx = fresh;
            *convo_ctx_len = cal_len;
        }
        return;
    }
    size_t old_len = *convo_ctx_len;
    size_t total = old_len + cal_len + 2;
    char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!merged)
        return;
    memcpy(merged, *convo_ctx, old_len);
    merged[old_len] = '\n';
    merged[old_len + 1] = '\n';
    memcpy(merged + old_len + 2, cal_buf, cal_len);
    merged[total] = '\0';
    alloc->free(alloc->ctx, *convo_ctx, old_len + 1);
    *convo_ctx = merged;
    *convo_ctx_len = total;
}

uint32_t hu_daemon_reply_budget(const hu_agent_t *agent, hu_channel_t *ch, const char *key,
                                size_t key_len, size_t combined_len, bool is_group,
                                bool brief_mode) {
    if (!agent)
        return 0;
    uint32_t max_chars = 0;
    if (ch && ch->vtable && ch->vtable->get_response_constraints) {
        hu_channel_response_constraints_t constraints = {0};
        if (ch->vtable->get_response_constraints(ch->ctx, &constraints) == HU_OK)
            max_chars = constraints.max_chars;
    }
    const hu_contact_profile_t *cp = (!is_group && agent->persona && key && key_len > 0)
                                         ? hu_persona_find_contact(agent->persona, key, key_len)
                                         : NULL;
    int calibrated = is_group ? hu_conversation_max_response_chars(combined_len)
                              : hu_conversation_max_response_chars_relational(
                                    combined_len, cp, agent->relationship.stage);
    if (calibrated > 0 && (max_chars == 0 || (uint32_t)calibrated < max_chars))
        max_chars = (uint32_t)calibrated;
    if (brief_mode) {
        uint32_t brief_cap =
            hu_conversation_brief_char_cap(is_group, cp, agent->relationship.stage);
        if (max_chars > brief_cap)
            max_chars = brief_cap;
    }
    return max_chars;
}
