/* src/daemon/daemon_followup_sched.c
 *
 * Follow-up watcher scheduling tick (S2.1b) — carved out of
 * hu_service_run_proactive_checkins (daemon.c) per the file-size-ceiling
 * ratchet. Behavior-preserving move plus the 2026-07-18 per-contact cooldown.
 *
 * For each iMessage channel and each persona contact with a warmth tier that
 * warrants follow-ups (CLOSE / FRIEND), check chat.db for a read-but-unreplied
 * outbound. If found and not already scheduled, compute a circadian-aware send
 * time and enqueue a template via the existing schedule API. The service
 * loop's flush_scheduled_for picks it up at the computed time.
 *
 * Dedup state is in-memory only (a daemon restart wipes it); worst case is one
 * duplicate follow-up across a restart. Two guards compose:
 *   - msg-id ring: don't re-schedule for the same unreplied message
 *   - per-contact cooldown ledger: don't bump the same contact more than once
 *     per HU_FOLLOWUP_PER_CONTACT_COOLDOWN_MS. The ring alone cannot bound
 *     frequency — each sent bump becomes a NEW read-but-unreplied message and
 *     re-triggers (2026-07-14 incident: identical bump 5x in one day).
 *
 * Per-contact chronotype is not yet a persona field, so we use the agent's own
 * chronotype as a proxy (close friends often share rhythms). Future
 * enhancement: hu_contact_profile.chronotype. */

#include "human/agent.h"
#include "human/context/conversation.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/daemon.h"
#include "human/follow_up.h"
#include "human/persona.h"

#ifdef HU_HAS_IMESSAGE
#include "human/channels/imessage.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

void hu_daemon_followup_sched_tick(hu_agent_t *agent, hu_service_channel_t *channels,
                                   size_t channel_count) {
    if (!agent || !agent->persona || !channels)
        return;

    static hu_followup_dedup_t followup_dedup;
    static hu_followup_contact_ledger_t followup_ledger;
    static bool followup_dedup_inited;
    if (!followup_dedup_inited) {
        hu_followup_dedup_init(&followup_dedup);
        hu_followup_contact_ledger_init(&followup_ledger);
        followup_dedup_inited = true;
    }

    time_t fnow_t = time(NULL);
    struct tm flocal_tm;
    int ftz_off = 0;
    if (localtime_r(&fnow_t, &flocal_tm))
        ftz_off = (int)flocal_tm.tm_gmtoff;

    for (size_t fc = 0; fc < channel_count; fc++) {
        if (!channels[fc].channel || !channels[fc].channel->vtable ||
            !channels[fc].channel->vtable->name)
            continue;
        const char *fch_name = channels[fc].channel->vtable->name(channels[fc].channel->ctx);
        if (!fch_name || strcmp(fch_name, "imessage") != 0)
            continue;

        for (size_t ci = 0; ci < agent->persona->contacts_count; ci++) {
            const hu_contact_profile_t *cp = &agent->persona->contacts[ci];
            if (!cp->contact_id || !cp->warmth_level)
                continue;

            hu_followup_warmth_t warmth = hu_followup_warmth_from_string(cp->warmth_level);
            if (warmth == HU_FOLLOWUP_WARMTH_NONE)
                continue;

            int64_t fmsg_id = 0;
            uint64_t fread_at_ms = 0;
#ifdef HU_HAS_IMESSAGE
            hu_error_t qerr = hu_imessage_find_unreplied_read(
                cp->contact_id, strlen(cp->contact_id), &fmsg_id, &fread_at_ms);
#else
            /* Builds without HU_HAS_IMESSAGE can't query chat.db. The outer
             * guard filters to imessage channels; we only reach this point
             * when imessage support is compiled in. Stub-out as NOT_SUPPORTED
             * so the symbol isn't required at link time. */
            hu_error_t qerr = HU_ERR_NOT_SUPPORTED;
            (void)cp;
            (void)fmsg_id;
            (void)fread_at_ms;
#endif
            if (qerr != HU_OK || fmsg_id == 0)
                continue;

            if (hu_followup_dedup_seen(&followup_dedup, fmsg_id))
                continue;

            if (hu_followup_contact_recent(&followup_ledger, cp->contact_id,
                                           (uint64_t)fnow_t * 1000ULL,
                                           HU_FOLLOWUP_PER_CONTACT_COOLDOWN_MS))
                continue;

            hu_followup_input_t fin = {
                .read_at_ms = fread_at_ms,
                .warmth = warmth,
                .contact_chronotype = agent->persona->chronotype,
                .local_tz_offset_seconds = ftz_off,
                .seed = (uint32_t)fmsg_id ^ (uint32_t)fnow_t,
            };
            hu_followup_decision_t fdec = hu_followup_decide(&fin);
            if (!fdec.should_schedule)
                continue;

            size_t tmpl_len = strlen(fdec.template_text);
            hu_error_t serr = hu_conversation_schedule_message_on(
                cp->contact_id, strlen(cp->contact_id), "imessage", 8, fdec.template_text, tmpl_len,
                fdec.send_at_ms);
            if (serr == HU_OK) {
                hu_followup_dedup_record(&followup_dedup, fmsg_id);
                hu_followup_contact_record(&followup_ledger, cp->contact_id,
                                           (uint64_t)fnow_t * 1000ULL);
                hu_log_info("human", agent->observer,
                            "scheduled follow-up: contact=%s msg_id=%lld send_at_ms=%llu warmth=%d",
                            cp->contact_id, (long long)fmsg_id, (unsigned long long)fdec.send_at_ms,
                            (int)warmth);
            }
        }
    }
}
