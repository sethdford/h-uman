/* daemon_reactive_calibration.c — step 2c of the reactive reply, moved verbatim
 * from daemon.c (2026-10-02) so the replay harness shares it.
 * Contract: include/human/daemon/reactive_calibration.h. */
#include "human/daemon/reactive_calibration.h"

#include "human/agent.h"
#include "human/agent/length_policy.h"
#include "human/channel.h"
#include "human/context/conversation.h"
#include "human/persona.h"
#include <string.h>

void hu_daemon_append_length_calibration(hu_allocator_t *alloc, hu_agent_t *agent, const char *key,
                                         size_t key_len, const char *combined, size_t combined_len,
                                         bool is_group, uint32_t turn_cap, char **convo_ctx,
                                         size_t *convo_ctx_len) {
    if (!alloc || !agent || !convo_ctx || !convo_ctx_len || !combined || combined_len == 0)
        return;
    char cal_buf[1024];
    const hu_contact_profile_t *cp_cal = (agent->persona && key && key_len > 0)
                                             ? hu_persona_find_contact(agent->persona, key, key_len)
                                             : NULL;
    size_t cal_len = hu_conversation_calibrate_length_capped(combined, combined_len, is_group,
                                                             cp_cal, agent->relationship.stage,
                                                             turn_cap, cal_buf, sizeof(cal_buf));
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

void hu_daemon_reply_budget(const hu_agent_t *agent, hu_channel_t *ch, const char *key,
                            size_t key_len, const char *combined, size_t combined_len,
                            bool is_group, bool brief_mode, hu_length_turn_result_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!agent)
        return;
    uint32_t channel_max = 0;
    if (ch && ch->vtable && ch->vtable->get_response_constraints) {
        hu_channel_response_constraints_t constraints = {0};
        if (ch->vtable->get_response_constraints(ch->ctx, &constraints) == HU_OK)
            channel_max = constraints.max_chars;
    }
    const hu_contact_profile_t *cp = (!is_group && agent->persona && key && key_len > 0)
                                         ? hu_persona_find_contact(agent->persona, key, key_len)
                                         : NULL;
    hu_length_turn_t turn = {.inbound = combined,
                             .inbound_len = combined_len,
                             .contact = cp,
                             .stage = agent->relationship.stage,
                             .channel_max = channel_max,
                             .is_group = is_group,
                             .brief_mode = brief_mode};
    hu_length_policy_turn(&turn, hu_length_policy_mode(), out);
}
