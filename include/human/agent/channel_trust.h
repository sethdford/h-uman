#ifndef HU_AGENT_CHANNEL_TRUST_H
#define HU_AGENT_CHANNEL_TRUST_H

#include "human/memory/trust.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * SOTA-2026 init-09 §2.10: channel → trust-tier mapping.
 *
 * Channel handlers MUST set `active_channel` to *qualified* strings
 * ("telegram_dm" vs "telegram_group"), not unqualified ones, so this
 * classifier can distinguish 1:1 sessions from group chats. The fallback
 * for an unqualified or unknown string is THIRD_PARTY — the conservative
 * default that forces re-validation in the recall verifier.
 */

/* Classify a channel string. Empty / NULL → THIRD_PARTY (safe default).
 * The classifier is case-insensitive on the ASCII letter prefix. */
hu_trust_tier_t hu_channel_trust(const char *channel, size_t channel_len);

/* True when the channel is a 1:1 session (DM, CLI, paired iMessage). */
bool hu_channel_is_one_to_one(const char *channel, size_t channel_len);

/* Build a fully-qualified provenance stamp for the active channel.
 * `handle` may be NULL for self-channels. */
hu_provenance_t hu_channel_trust_stamp(const char *channel, size_t channel_len,
                                       const char *handle, size_t handle_len,
                                       int64_t now_ts);

#endif /* HU_AGENT_CHANNEL_TRUST_H */
