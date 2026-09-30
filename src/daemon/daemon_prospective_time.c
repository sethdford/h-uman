/*
 * src/daemon/daemon_prospective_time.c — time-cued follow-ups for the
 * proactive tick. See include/human/daemon/prospective_time.h.
 *
 * Both producers are the hu_service_run bodies as of b622a96d4, moved
 * verbatim apart from the out-parameters (commitment_ctx / commitment_ids /
 * due_fu_buf / due_followup_id_listed were locals there).
 */
#include "human/daemon/prospective_time.h"

#include "human/agent.h"
#include "human/channel.h"
#include "human/memory/superhuman.h"

#include <stdio.h>
#include <string.h>

void hu_daemon_prospective_commitment_ctx(hu_allocator_t *alloc, struct hu_agent *agent,
                                          const char *contact_id, int64_t now, char **ctx_out,
                                          size_t *ctx_len_out, int64_t ids_out[3],
                                          size_t *ids_count_out) {
    if (ctx_out)
        *ctx_out = NULL;
    if (ctx_len_out)
        *ctx_len_out = 0;
    if (ids_count_out)
        *ids_count_out = 0;
    if (!alloc || !agent || !agent->memory || !contact_id || !ctx_out || !ctx_len_out || !ids_out ||
        !ids_count_out)
        return;
    hu_superhuman_commitment_t *due = NULL;
    size_t due_count = 0;
    if (hu_superhuman_commitment_list_due(agent->memory, alloc, now, 3, &due, &due_count) !=
            HU_OK ||
        !due || due_count == 0)
        return;
    size_t cid_len = strlen(contact_id);
    char ctx_buf[1024];
    size_t ctx_pos = 0;
    for (size_t di = 0; di < due_count && ctx_pos < sizeof(ctx_buf) - 200; di++) {
        if (cid_len != strlen(due[di].contact_id) ||
            memcmp(due[di].contact_id, contact_id, cid_len) != 0)
            continue;
        int n = snprintf(ctx_buf + ctx_pos, sizeof(ctx_buf) - ctx_pos,
                         "COMMITMENT FOLLOW-UP: %s was due. Ask if it happened: "
                         "'hey did you ever %s?'\n",
                         due[di].description, due[di].description);
        if (n > 0 && ctx_pos + (size_t)n < sizeof(ctx_buf)) {
            ctx_pos += (size_t)n;
            if (*ids_count_out < 3)
                ids_out[(*ids_count_out)++] = due[di].id;
        }
    }
    if (ctx_pos > 0) {
        char *c = (char *)alloc->alloc(alloc->ctx, ctx_pos + 1);
        if (c) {
            memcpy(c, ctx_buf, ctx_pos);
            c[ctx_pos] = '\0';
            *ctx_out = c;
            *ctx_len_out = ctx_pos;
        }
    }
    hu_superhuman_commitment_free(alloc, due, due_count);
}

/* Items are listed, not marked sent: mark-sent stays tied to an actual send
 * (the F31 path at the send site), so an unsent item correctly reappears. */
static size_t pm_legacy_due_followups(hu_allocator_t *alloc, struct hu_agent *agent,
                                      const char *contact_id, int64_t now, char *buf, size_t cap,
                                      int64_t *listed_id) {
    hu_delayed_followup_t *due_arr = NULL;
    size_t due_n = 0;
    if (hu_superhuman_delayed_followup_list_due(agent->memory, alloc, now, &due_arr, &due_n) !=
            HU_OK ||
        !due_arr || due_n == 0)
        return 0;
    size_t pos = 0;
    size_t listed = 0;
    for (size_t fi = 0; fi < due_n && listed < 1; fi++) {
        if (strcmp(due_arr[fi].contact_id, contact_id) != 0)
            continue;
        if (listed_id)
            *listed_id = due_arr[fi].id;
        int w = snprintf(buf + pos, cap - pos, "- %s (due %llds ago)\n", due_arr[fi].topic,
                         (long long)(now - due_arr[fi].scheduled_at));
        if (w <= 0 || (size_t)w >= cap - pos)
            break;
        pos += (size_t)w;
        listed++;
    }
    hu_superhuman_delayed_followup_free(alloc, due_arr, due_n);
    return pos;
}

size_t hu_daemon_prospective_due_followups(hu_allocator_t *alloc, struct hu_agent *agent,
                                           struct hu_channel *ch, const char *target,
                                           size_t target_len, const char *contact_id, int64_t now,
                                           char *buf, size_t cap, int64_t *listed_id) {
    (void)ch;
    (void)target;
    (void)target_len;
    if (buf && cap > 0)
        buf[0] = '\0';
    if (!alloc || !agent || !agent->memory || !contact_id || !buf || cap == 0)
        return 0;
    return pm_legacy_due_followups(alloc, agent, contact_id, now, buf, cap, listed_id);
}
