#ifndef HU_MEMORY_PROVENANCE_H
#define HU_MEMORY_PROVENANCE_H

/* G0 — Ingest-path provenance (init-09 anti-MINJA gate).
 *
 * hu_provenance_t is a plain-old-data value type that stamps the origin of a
 * memory-ingest candidate.  It carries:
 *
 *   source     — trust tier drawn from hu_write_source_t (write_trust.h).
 *                This is the single field the write-time gate evaluates.
 *   channel[]  — NUL-terminated channel name ("cli", "telegram", "slack",
 *                "imessage", "discord", "feed-web", …).  Empty when unknown.
 *   sender[]   — NUL-terminated sender handle or display name.  Empty when
 *                the content comes directly from the local user (no channel).
 *   observed_at — unix ms when the content was received.
 *
 * Lifetime contract: channel[] and sender[] are copied into the struct at
 * construction time (strncpy with NUL-padding).  The struct is stack-allocated
 * and requires no heap allocation or free call.
 *
 * Performance: the struct is 16 + 64 + 64 = 144 bytes on all platforms.
 * Passing by const pointer on the hot ingest path avoids the copy cost while
 * keeping the caller in charge of lifetime.
 */

#include "human/memory/write_trust.h" /* hu_write_source_t */
#include <stdint.h>
#include <string.h>

/* ── struct ────────────────────────────────────────────────────────────── */

typedef struct hu_provenance {
    hu_write_source_t source;     /* trust tier */
    int64_t          observed_at;
    char             channel[64]; /* origin channel name, NUL-terminated */
    char             sender[64];  /* sender handle, NUL-terminated; empty = local user */

    /* Rate-limit hint for the write-trust gate.  Production code leaves both
     * at 0 (no enforcement until a per-source write tracker is wired in).
     * Tests set recent_writes >> rate_limit to simulate a flooding attack.
     *
     * When rate_limit == 0, hu_write_trust_score treats anomaly_score as 1.0
     * (no penalty) regardless of recent_writes.  Setting rate_limit to a
     * positive value activates the anomaly floor. */
    uint32_t         recent_writes; /* writes from this source in current window */
    uint32_t         rate_limit;    /* trip threshold; 0 = disabled */
} hu_provenance_t;

/* ── helper constructors (inline — zero allocation) ─────────────────── */

/* Direct local-user input (CLI, onboard wizard, API with full trust).
 * Maps to HU_WRITE_SOURCE_USER — no gate needed. */
static inline hu_provenance_t hu_provenance_user_direct(int64_t observed_at) {
    hu_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.source = HU_WRITE_SOURCE_USER;
    p.observed_at = observed_at;
    strncpy(p.channel, "cli", sizeof(p.channel) - 1);
    /* sender[] left empty — direct user, no third-party handle */
    return p;
}

/* Message arriving via a named messaging channel (Telegram, Slack, iMessage,
 * Discord, etc.).  The trust tier is derived from the channel name:
 *
 *   "imessage", "slack", "telegram", "discord", "email" — CHANNEL_TRUSTED
 *       (these channels require pairing / OAuth so the sender identity is
 *       authenticated at the transport layer).
 *
 *   All other channel names                              — CHANNEL_OPEN
 *       (public webhooks, anonymous feeds, unknown).
 *
 * Both CHANNEL_TRUSTED and CHANNEL_OPEN are below HU_WRITE_SOURCE_USER and
 * therefore route through the write-trust gate inside hu_personal_model_ingest.
 *
 * channel_name must not be NULL; sender may be NULL (stored as "").
 */
static inline hu_provenance_t hu_provenance_from_channel(const char *channel_name,
                                                         const char *sender,
                                                         int64_t     observed_at) {
    hu_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.observed_at = observed_at;

    /* Derive trust tier from channel name. */
    if (channel_name) {
        if (strcmp(channel_name, "imessage") == 0 ||
            strcmp(channel_name, "slack")    == 0 ||
            strcmp(channel_name, "telegram") == 0 ||
            strcmp(channel_name, "discord")  == 0 ||
            strcmp(channel_name, "email")    == 0) {
            p.source = HU_WRITE_SOURCE_CHANNEL_TRUSTED;
        } else {
            p.source = HU_WRITE_SOURCE_CHANNEL_OPEN;
        }
        strncpy(p.channel, channel_name, sizeof(p.channel) - 1);
    } else {
        p.source = HU_WRITE_SOURCE_CHANNEL_OPEN;
    }

    if (sender)
        strncpy(p.sender, sender, sizeof(p.sender) - 1);

    return p;
}

/* Agent self-ingest: the agent is ingesting its own output for style learning.
 * Maps to HU_WRITE_SOURCE_AGENT.  hu_personal_model_ingest returns HU_OK
 * immediately for this tier without extracting user facts, consistent with
 * the pre-G0 from_user=false path. */
static inline hu_provenance_t hu_provenance_self(int64_t observed_at) {
    hu_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.source = HU_WRITE_SOURCE_AGENT;
    p.observed_at = observed_at;
    strncpy(p.channel, "self", sizeof(p.channel) - 1);
    return p;
}

/* Feed / file ingest (document, RSS, scraped web). */
static inline hu_provenance_t hu_provenance_feed(const char *feed_label, int64_t observed_at) {
    hu_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.source = HU_WRITE_SOURCE_FEED_WEB;
    p.observed_at = observed_at;
    if (feed_label)
        strncpy(p.channel, feed_label, sizeof(p.channel) - 1);
    return p;
}

#endif /* HU_MEMORY_PROVENANCE_H */
