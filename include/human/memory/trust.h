#ifndef HU_MEMORY_TRUST_H
#define HU_MEMORY_TRUST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * SOTA-2026 init-09: trust tiers + provenance.
 *
 * Per-memory and per-fact trust classification. Higher numeric value =
 * higher authority. Compare with `>=` against a threshold — never with
 * `==` or `<` against a hardcoded numeric value.
 *
 * The ordinals are LOCKED across the SOTA-2026 fleet (initiatives
 * #04, #05, #08, #09, #10): see
 *   docs/plans/2026-05-11-sota-2026-massive-team-program.md
 * "Locked conventions" section. Do NOT renumber.
 */
typedef enum hu_trust_tier {
    HU_TRUST_UNTRUSTED       = 0, /* unknown origin or quarantined */
    HU_TRUST_THIRD_PARTY     = 1, /* group chat, RSS, feed, stranger email */
    HU_TRUST_FIRST_PARTY     = 2, /* user-installed tool / 1st-party source */
    HU_TRUST_PERSONA_DERIVED = 3, /* computed from the user's own outputs */
    HU_TRUST_USER_DIRECT     = 4, /* user typed it into a 1:1 session */
} hu_trust_tier_t;

/* Sentinel: max trust a third-party source may ever assert. */
#define HU_TRUST_THIRD_PARTY_MAX HU_TRUST_THIRD_PARTY

#define HU_PROV_CHANNEL_MAX 64
#define HU_PROV_HANDLE_MAX 128

/* Per-fact / per-memory provenance stamp. Embedded by value (no pointers)
 * so a stack-local hu_provenance_t can be safely copied into a heap or
 * value-type owner without dangling-pointer risk. */
typedef struct hu_provenance {
    hu_trust_tier_t tier;
    char channel[HU_PROV_CHANNEL_MAX];     /* "cli", "telegram_dm", "feed:rss", ... */
    char contact_handle[HU_PROV_HANDLE_MAX]; /* sender handle; empty for self */
    int64_t source_ts;                     /* when the original content arrived */
} hu_provenance_t;

/* True when src may overwrite a stored fact whose current trust is tgt.
 * Convention is `>=`: same-tier refresh is allowed; lower-tier overwrite
 * is forbidden. */
static inline bool hu_trust_can_overwrite(hu_trust_tier_t src, hu_trust_tier_t tgt) {
    return (int)src >= (int)tgt;
}

/* Build a default-USER_DIRECT provenance (used when callers pass NULL,
 * typically inside #ifdef _HU_PM_SELF_TEST blocks). */
static inline hu_provenance_t hu_provenance_user_direct(int64_t ts) {
    hu_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.tier = HU_TRUST_USER_DIRECT;
    /* memcpy of small literal — channel/handle stay zero-padded but
     * the channel string is the most common identifier. */
    const char cli[] = "cli";
    memcpy(p.channel, cli, sizeof(cli));
    p.source_ts = ts;
    return p;
}

/* Build a provenance with explicit channel + tier. Truncates safely to
 * the embedded buffers; handle may be NULL. */
static inline hu_provenance_t hu_provenance_make(hu_trust_tier_t tier,
                                                 const char *channel,
                                                 const char *handle,
                                                 int64_t ts) {
    hu_provenance_t p;
    memset(&p, 0, sizeof(p));
    p.tier = tier;
    if (channel) {
        size_t n = strlen(channel);
        if (n >= sizeof(p.channel))
            n = sizeof(p.channel) - 1;
        memcpy(p.channel, channel, n);
    }
    if (handle) {
        size_t n = strlen(handle);
        if (n >= sizeof(p.contact_handle))
            n = sizeof(p.contact_handle) - 1;
        memcpy(p.contact_handle, handle, n);
    }
    p.source_ts = ts;
    return p;
}

#endif /* HU_MEMORY_TRUST_H */
