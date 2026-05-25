#ifndef HU_DAEMON_PROACTIVE_H
#define HU_DAEMON_PROACTIVE_H

#include "core/allocator.h"
#include "core/error.h"
#include "daemon.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct hu_agent;
struct hu_contact_profile;
struct hu_legacy_memory;
struct hu_memory_vtable;

/**
 * daemon_proactive.h — Proactive check-in subsystem extracted from daemon.c.
 *
 * Manages contact activity tracking (LRU cache), proactive route parsing,
 * prompt construction, and the main check-in orchestration loop.
 */

/* ── Contact activity LRU cache ─────────────────────────────────────── */

#define HU_DAEMON_CONTACT_ACTIVITY_CAP 256
#define HU_DAEMON_ACTIVITY_FRESH_SECS  (48 * 3600)

typedef struct hu_daemon_contact_activity {
    char contact_id[128];
    char last_channel[64];
    char last_session_key[128];
    time_t last_activity;
    uint64_t lru_seq;
} hu_daemon_contact_activity_t;

/**
 * hu_proactive_context_t — Module state for proactive check-in subsystem.
 *
 * Thread safety: NOT THREAD SAFE. Caller must ensure single-threaded access.
 *   All mutating functions (reset, record, apply_route) assert non-reentrancy
 *   in debug builds.
 *
 * Lifecycle:
 *   - Init:  zero-init with memset or = {0}
 *   - Clear: hu_proactive_context_reset() zeros all fields
 *   - Bounds: entries[] is a fixed-size LRU cache (HU_DAEMON_CONTACT_ACTIVITY_CAP)
 */
typedef struct hu_proactive_context {
    hu_daemon_contact_activity_t entries[HU_DAEMON_CONTACT_ACTIVITY_CAP];
    size_t count;
    uint64_t seq;
} hu_proactive_context_t;

/** Zero all fields in the proactive context (reset the LRU cache). */
void hu_proactive_context_reset(hu_proactive_context_t *ctx);

/** Record inbound activity for a contact on a given channel/session. */
void hu_daemon_contact_activity_record(hu_proactive_context_t *ctx, const char *contact_id,
                                       const char *channel_name, const char *session_key);

/** Check if a channel name exists in the service channel list. */
bool hu_daemon_channel_list_has_name(const hu_service_channel_t *channels, size_t channel_count,
                                     const char *name);

/** Parse proactive_channel string into channel and target route buffers. */
void hu_daemon_proactive_parse_route(const struct hu_contact_profile *cp, char *ch_buf,
                                     char *target_buf);

/** Apply recent activity routing override if contact has fresh inbound activity. */
void hu_daemon_proactive_apply_route(hu_proactive_context_t *ctx, const char *contact_id,
                                     time_t now, const hu_service_channel_t *channels,
                                     size_t channel_count, char *ch_buf, char *target_buf,
                                     size_t *target_len);

/** Build memory callback context for a contact (recalls + degradation + protective filter).
 *  Used by both proactive prompt builder and daemon main loop. */
char *hu_daemon_build_callback_context(hu_allocator_t *alloc, struct hu_legacy_memory *memory,
                                       const char *session_id, size_t session_id_len,
                                       const char *msg, size_t msg_len, size_t *out_len,
                                       struct hu_agent *agent);

/** Build proactive prompt for a contact with memory context, weather, feeds, calendar. */
char *hu_daemon_proactive_prompt_for_contact(hu_allocator_t *alloc, struct hu_agent *agent,
                                             struct hu_legacy_memory *memory,
                                             const struct hu_contact_profile *cp, size_t *out_len);

/* P2-5 (2026-05-16): outbound-safety predicate. Returns false when content
 * is unsafe to inject into an outbound proactive prompt (first-person
 * pronouns, confession verbs, charged emotion keywords, format-specifier
 * injection, newlines). See src/daemon_proactive.c for the contract. */
bool hu_daemon_callback_content_is_safe(const char *content, size_t content_len);

/* Forward declare throttle type. */
struct hu_proactive_throttle;

/* Follow-up watcher flush function (US-48-3). Generates a follow-up draft
 * for a contact and sends it via iMessage if throttle allows. Called by
 * hu_daemon_tick_follow_up_watcher() when a scheduled follow-up is ready. */
hu_error_t hu_daemon_follow_up_flush_for_contact(hu_allocator_t *alloc, struct hu_agent *agent,
                                                 const char *contact_handle, struct hu_config *cfg,
                                                 hu_service_channel_t *channels,
                                                 size_t channel_count,
                                                 struct hu_proactive_throttle *throttle);

#endif /* HU_DAEMON_PROACTIVE_H */
