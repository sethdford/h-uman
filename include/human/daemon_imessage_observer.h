/* include/human/daemon_imessage_observer.h
 *
 * Phase 3 of docs/plans/2026-05-18-imessage-sota.md completion: a daemon
 * tick that polls chat.db for the enriched events that the regular
 * iMessage channel poll does NOT read — audio-message transcripts,
 * message edits, group-chat events, and balloon-plugin payloads — and
 * feeds them into hu_personal_model_t via the existing ingest wrappers.
 *
 * Mirrors src/daemon_reaction_poll.c's tick + wire pattern. Independent
 * from reaction_poll because the SQL is different (we look at the message
 * row itself, not its associated_message_type 2xxx/3xxx tapback rows) and
 * the watermark advances on m.date for any message, not just tapbacks. */

#ifndef HU_DAEMON_IMESSAGE_OBSERVER_H
#define HU_DAEMON_IMESSAGE_OBSERVER_H

#include "human/config.h"
#include "human/core/error.h"

#include <stddef.h>
#include <stdint.h>

/* Forward decl avoids pulling memory subsystem types into callers. */
struct hu_personal_model;

#ifdef __cplusplus
extern "C" {
#endif

/* Daemon-init wiring: attach the personal_model the observer will ingest
 * into. Pass NULL at shutdown to detach. Mirrors the
 * hu_daemon_reaction_wire_personal_model pattern. */
void hu_daemon_imessage_observer_wire_personal_model(struct hu_personal_model *model);

/* Single tick — pulls every chat.db message with date > `since_unix`,
 * dispatches to per-feature ingest paths:
 *
 *   audio transcript:    balloon_bundle_id matches AudioMessage AND payload_data
 *                        non-NULL → hu_imessage_extract_audio_transcript →
 *                        hu_imessage_ingest_balloon (kind=AUDIO_TRANSCRIPT)
 *   edit history:        message_summary_info non-NULL (date_edited > 0) →
 *                        hu_imessage_extract_edit_chain → hu_imessage_ingest_edit
 *                        for each delta
 *   group event:         group_action_type non-NULL → narration ingest
 *   generic balloon:     balloon_bundle_id non-NULL (any kind we recognize) →
 *                        hu_imessage_ingest_balloon (kind from classifier)
 *
 * Schema-tolerant: missing columns (e.g. pre-Ventura `message_summary_info`)
 * are silently skipped — the SQL builder probes column availability once
 * per-process via PRAGMA table_info and caches the result.
 *
 * `*watermark_inout` is updated to the max(date) of processed rows so the
 * caller can persist for restart. */
hu_error_t hu_daemon_imessage_observer_tick(const hu_config_t *cfg, int64_t since_unix,
                                            int64_t *watermark_inout, size_t *out_ingested);

/* Interval-gated tick used by the daemon main loop. Same shape as
 * hu_daemon_tick_reaction_poll. */
hu_error_t hu_daemon_tick_imessage_observer(const hu_config_t *cfg, int64_t now_unix,
                                            int64_t *last_poll_unix_inout,
                                            int64_t *watermark_inout);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_IMESSAGE_OBSERVER_H */
