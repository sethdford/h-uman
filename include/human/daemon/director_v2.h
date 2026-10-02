#ifndef HU_DAEMON_DIRECTOR_V2_H
#define HU_DAEMON_DIRECTOR_V2_H

/* Director v2 (HU_DIRECTOR_V2=off|shadow|live, default off).
 *
 * The v1 director (daemon_director.c) asks for brevity on almost every turn:
 * in prod 2026-09-17..10-01, 335 of 456 decisions carried a length or
 * deflection cue and 108 were tapback-only, including a parent's covid news
 * and "your AI is messed up?". v2 directs intent, tone and what to engage
 * with, never length; sees 12 labelled messages, a one-line contact summary
 * and Seth's LEARNED tapback behaviour for this contact and message shape
 * (director_tapback.h). A tapback-only choice becomes text only when that
 * learned data says he almost never does it here; no data, no override.
 *
 * OFF: exactly hu_daemon_director_call. SHADOW: v1 decides; v2 runs off the
 * reply path, loopback providers only. LIVE: v2 decides (v1 if v2 fails).
 * Guide: docs/guides/director-v2.md. */

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/daemon/director.h"
#include "human/daemon/director_tapback.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

struct hu_agent;
struct hu_contact_profile;

/* Prompt budgets, enforced by tests. */
#define HU_DIRECTOR_V2_SYSTEM_CAP 2560u /* system prompt incl. the forms block */
#define HU_DIRECTOR_V2_USER_CAP   2560u /* per-turn context */
#define HU_DIRECTOR_V2_HISTORY    12u   /* thread messages the director sees */

/* MEASUREMENT ONLY (the shadow log and the tests), never a behaviour rule.
 * What a direction asks for, as a bitmask. LENGTH: a length cue ("one line",
 * "a few words", "brief", "5 words", ...). DEFLECT: a dodge ("keep it light",
 * "non-committal", "don't over-explain", "laugh it off", "deflect"). On the
 * 2026-09-17..10-01 prod log it flags 335 of 456 director decisions. */
#define HU_DIRECTIVE_LENGTH  0x1u
#define HU_DIRECTIVE_DEFLECT 0x2u
unsigned hu_director_directive_flags(const char *dir, size_t len);

/* Brevity-or-deflection: flags != 0. NULL/empty is false. */
bool hu_director_brevity_directive(const char *dir, size_t len);

/* v2 system prompt (plus the forms block when HU_DIRECTOR_FORMS is LIVE).
 * Length written, 0 if it did not fit. */
size_t hu_director_v2_system_prompt(char *buf, size_t cap);

/* Per-turn context: an optional "Contact:" line (relationship, dunbar layer;
 * never a name), the optional learned "How Seth reacts" facts line
 * (hu_tapback_profile_facts), the last HU_DIRECTOR_V2_HISTORY entries labelled
 * Seth/Them, the new message and the optional situation line. Length written. */
size_t hu_director_v2_user_prompt(char *buf, size_t cap, const struct hu_contact_profile *cp,
                                  const hu_channel_history_entry_t *entries, size_t entry_count,
                                  const char *combined, size_t combined_len, const char *situation,
                                  const char *facts);

/* One v2 director call on `provider`; parses with hu_daemon_parse_director_result,
 * then applies the learned post-check hu_director_v2_tapback_check(tp) (NULL tp:
 * no data, the model decides). *src and *prompt_bytes (system + user) are
 * optional. Returns true if result is valid. */
bool hu_director_v2_call(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                         size_t model_len, const struct hu_contact_profile *cp,
                         const hu_channel_history_entry_t *entries, size_t entry_count,
                         const char *combined, size_t combined_len, const char *situation,
                         const hu_tapback_profile_t *tp, const char *facts,
                         hu_director_result_t *result, hu_tapback_src_t *src, size_t *prompt_bytes);

/* The endpoint the director's provider sends the thread to: under #587
 * local_only it borrows the agent's provider (config default_provider's
 * base_url); otherwise its own named entry (today "gemini"; NULL = Vertex).
 * NULL when unknown. */
const char *hu_director_v2_endpoint(const struct hu_agent *agent, const hu_provider_t *provider);

/* True only when that endpoint is known and loopback. Unknown is not local. */
bool hu_director_v2_endpoint_is_local(const struct hu_agent *agent, const hu_provider_t *provider);

/* Test seam: wait up to timeout_ms for the SHADOW worker to finish. False on
 * timeout. */
bool hu_director_v2_shadow_drain(unsigned timeout_ms);

/* The daemon's director entry point.
 *   OFF:    exactly hu_daemon_director_call.
 *   SHADOW: v1 decides and returns at once; v2 runs on a detached worker
 *           (one in flight; otherwise logged v2=skipped_busy), and only when
 *           the director endpoint is loopback (else v2=skipped_nonlocal).
 *   LIVE:   v2 decides inline (12-message read, learned profile, call); v1
 *           if v2 fails. */
bool hu_director_v2_decide(hu_allocator_t *alloc, struct hu_agent *agent, hu_channel_t *channel,
                           const char *key, size_t key_len, const char *combined,
                           size_t combined_len, const hu_channel_history_entry_t *entries,
                           size_t entry_count, const char *situation, hu_director_result_t *result);

#endif /* HU_DAEMON_DIRECTOR_V2_H */
