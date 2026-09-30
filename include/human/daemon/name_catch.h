#ifndef HU_DAEMON_NAME_CATCH_H
#define HU_DAEMON_NAME_CATCH_H

#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/memory/graph.h"
#include "human/memory/name_extract.h"
#include <stddef.h>

/* Per-turn name catcher (spec 2026-09-29 §4.2). Zero-model. Reads the
 * CONTACT's inbound text only — never the generated reply, so the daemon
 * cannot reinforce its own hallucinations. In LIVE it bumps known names
 * (freshness) and records new Capitalized names as UNKNOWN entities with
 * provenance "names:turn". */

#define HU_NAME_CATCH_PROVENANCE  "names:turn"
#define HU_NAME_CATCH_CONFIDENCE  0.3f
#define HU_NAME_CATCH_MAX         8   /* candidates per inbound message */
#define HU_NAME_CATCH_KNOWN_LIMIT 256 /* known names considered per contact */

typedef enum hu_name_catch_action {
    HU_NAME_CATCH_SKIP = 0,
    HU_NAME_CATCH_BUMP,   /* known name: hu_graph_upsert_entity freshness bump */
    HU_NAME_CATCH_INSERT, /* new name: typed upsert, UNKNOWN, names:turn, 0.3 */
} hu_name_catch_action_t;

typedef struct hu_name_catch_counts {
    size_t known;   /* KNOWN candidates seen */
    size_t fresh;   /* CAPITALIZED (new) candidates seen */
    size_t written; /* graph writes that succeeded (LIVE only) */
} hu_name_catch_counts_t;

struct hu_config;

/* Pure: may the catcher read this inbound message at all? Never a group chat
 * (other people's names and chatter), and never Seth's own text: a message
 * whose session is the configured self-chat handle
 * (channels.imessage.loopback_handle, the only from-me rows the poll
 * admits). `config` may be NULL. */
bool hu_name_catch_eligible(const hu_channel_loop_msg_t *msg, const struct hu_config *config);

/* HU_NAME_CATCH per hu_gate_mode_parse; unset -> OFF. */
hu_gate_mode_t hu_name_catch_mode(void);

/* Pure write decision: OFF and SHADOW never write; LIVE bumps a KNOWN name
 * and inserts a CAPITALIZED one. */
hu_name_catch_action_t hu_name_catch_action(hu_gate_mode_t mode, hu_name_kind_t kind);

/* One inbound message for `contact_id` at `mode`. Known names are the
 * contact's top HU_NAME_CATCH_KNOWN_LIMIT entities by mentions that
 * hu_name_entity_is_nameable accepts. `out` is always written. Returns the
 * first graph-write error (later candidates are still attempted). */
hu_error_t hu_daemon_name_catch(hu_allocator_t *alloc, hu_graph_t *g, hu_gate_mode_t mode,
                                const char *contact_id, size_t contact_id_len, const char *inbound,
                                size_t inbound_len, hu_name_catch_counts_t *out);

/* Daemon entry point: resolves HU_NAME_CATCH, logs once per process whether
 * the catcher is disabled (naming the env key) or active, logs
 * "name_catch shadow: known=%zu new=%zu (not written)" in SHADOW, and logs
 * and swallows errors so a reply is never blocked. */
void hu_daemon_name_catch_tick(hu_allocator_t *alloc, hu_graph_t *g, const char *contact_id,
                               size_t contact_id_len, const char *inbound, size_t inbound_len);

/* Daemon batch wiring: one reply batch msgs[start..end] (inclusive, all from
 * msgs[start].session_key, the contact id). Skips the whole batch unless
 * hu_name_catch_eligible(&msgs[start], config); otherwise feeds each
 * message's raw inbound `content` (never the reply or the combined prompt
 * text) to hu_daemon_name_catch_tick. The daemon calls it after the send, only
 * when a reply went out. NULL msgs/graph or start > end is a no-op. */
void hu_daemon_name_catch_batch(hu_allocator_t *alloc, hu_graph_t *graph,
                                const hu_channel_loop_msg_t *msgs, size_t start, size_t end,
                                const struct hu_config *config);

#endif /* HU_DAEMON_NAME_CATCH_H */
