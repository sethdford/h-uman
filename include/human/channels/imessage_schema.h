#ifndef HU_CHANNELS_IMESSAGE_SCHEMA_H
#define HU_CHANNELS_IMESSAGE_SCHEMA_H

/* Phase 6 of docs/plans/2026-05-18-imessage-sota.md: schema-version probe.
 *
 * Apple ships chat.db schema deltas every macOS release. Existing readers
 * do ad-hoc runtime fallback (imessage_reactions.c probes for sql_v17
 * vs sql_legacy). This module centralizes the question — given a chat.db,
 * which columns does its `message` table have? — into one cached probe.
 *
 * The probe also functions as a drift canary: any column we don't
 * recognize is captured into unknown_columns[] and surfaced at startup,
 * so Apple-added columns don't silently sit unhandled.
 *
 * The probe caches by db_path so subsequent calls are O(1). Reset the
 * cache with hu_imessage_schema_reset_cache() in tests or after a known
 * schema upgrade. */

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_IMESSAGE_SCHEMA_UNKNOWN_MAX         8
#define HU_IMESSAGE_SCHEMA_UNKNOWN_NAME_MAX    64
#define HU_IMESSAGE_SCHEMA_FINGERPRINT_HEX_LEN 65 /* 64 hex + NUL */

typedef struct {
    bool probed; /* false until first successful probe */

    /* message-table column presence */
    bool has_date_retracted;           /* macOS Ventura+ */
    bool has_thread_originator_guid;   /* macOS Ventura+ */
    bool has_associated_message_emoji; /* macOS Sonoma / iOS 17+ */
    bool has_group_action_type;        /* macOS Big Sur+ */
    bool has_group_title;              /* macOS Big Sur+ */
    bool has_balloon_bundle_id;        /* macOS Big Sur+ */
    bool has_expressive_send_style_id; /* macOS Big Sur+ */
    bool has_payload_data;             /* macOS Big Sur+ */
    bool has_message_summary_info;     /* macOS Ventura+ */

    /* drift canary: columns we don't recognize */
    char unknown_columns[HU_IMESSAGE_SCHEMA_UNKNOWN_MAX][HU_IMESSAGE_SCHEMA_UNKNOWN_NAME_MAX];
    size_t unknown_column_count;

    /* SHA-256 hex of the sorted column-name list — stable across runs of
     * the same schema. Used as a single compare-value when checking whether
     * a previously-seen schema is the same. */
    char schema_fingerprint[HU_IMESSAGE_SCHEMA_FINGERPRINT_HEX_LEN];
} hu_imessage_schema_caps_t;

/** Probe `chat.db` at the given path. Caches by path; subsequent calls with
 *  the same path return the cached result in O(1). Pass NULL `db_path` to
 *  use the default (~/Library/Messages/chat.db).
 *
 *  Returns HU_OK on success, HU_ERR_INVALID_ARGUMENT if out is NULL,
 *  HU_ERR_IO if the chat.db can't be opened or PRAGMA fails,
 *  HU_ERR_NOT_SUPPORTED on builds without SQLite. */
hu_error_t hu_imessage_schema_probe(const char *db_path, hu_imessage_schema_caps_t *out);

/** Reset the probe cache. Tests should call this between cases. */
void hu_imessage_schema_reset_cache(void);

/** Log the schema fingerprint + any unknown columns. Call once at daemon
 *  startup so operators see the schema state on every launch. If unknown
 *  columns are present, emits a separate WARN-level line so schema drift
 *  is visible before features silently break. Pattern mirrors the
 *  silent-config-gated-subsystems.md rule. */
void hu_imessage_schema_log_fingerprint(const hu_imessage_schema_caps_t *caps);

/* Resolve a handle (phone/email) to the chat GUID owning its most recent
 * message. The IMCore bridge verbs (`imsg send-rich --chat`, tapback, edit,
 * unsend) address chats by GUID. Returns false when unavailable.
 * macOS non-test builds only; a stub elsewhere. */
bool hu_imessage_reply_chat_guid_for_handle(const char *handle, size_t handle_len, char *out,
                                            size_t out_cap);

#endif /* HU_CHANNELS_IMESSAGE_SCHEMA_H */
