#ifndef HU_SECURITY_MODERATION_CONTEXT_H
#define HU_SECURITY_MODERATION_CONTEXT_H

/*
 * HU_MODERATION_CONTEXT=off|shadow|live (default off).
 *
 * SHIELD-004 (src/agent/agent_turn.c) replaces the twin's OWN outbound reply
 * with a canned decline whenever the static keyword list in
 * src/security/moderation.c (mod_violence_hit) matches a word-bounded "kill"
 * / "murder" / "violence". That list has no idiom/hyperbole/sport/media
 * exception, so "you killed it!", "traffic is killing me", "i'd kill for a
 * burrito" and "that murder podcast" all trip the canned "rather not get
 * into that one" — an obvious AI tell, confirmed firing on real replies in
 * production (2026-09-20, 2026-10-01).
 *
 * This module adds a second, LOCAL-model opinion on a violence-only hit
 * (hate and self_harm are untouched — owner ruling: no new static behaviour
 * rules, no new canned replies beyond the existing safety floors):
 *
 *   off    (default) — byte-identical to today. The keyword hit alone
 *          decides; *response is always replaced on a violence/hate flag.
 *   shadow — the keyword hit still decides (unchanged output), but this
 *          judge also runs so its accuracy can be measured. ONE aggregate
 *          line per hit: "[moderation_context shadow] verdict=idiom|real|error
 *          ms=N" (enums + a latency only — never message text).
 *   live   — a verdict of IDIOM keeps the reply unchanged (the bug fix). A
 *          verdict of REAL, or the judge being unavailable / timing out /
 *          answering something unparseable, falls back to today's behaviour
 *          (fail closed to the existing safe decline) — see
 *          docs/guides/moderation-context.md for why a REGENERATE path
 *          (src/agent/outbound/moderation.c's verdict for the proactive/F25/
 *          temporal/scheduled pipeline) is not reachable from this call site.
 *
 * The judge is one local-model call, loopback only (hu_proposer_context_
 * local_provider — the same resolution the commitment guard uses: the
 * reliable wrapper's primary alone, never a cloud fallback), tagged
 * X-HU-Purpose: moderation_check (foreground — this is on the reply's
 * critical path, so no X-HU-Priority header). A missing/non-loopback
 * provider, a transport error, or an answer that is not exactly one
 * recognizable word is HU_MOD_CTX_ERROR (fail closed).
 *
 * The decision itself (hu_moderation_context_keep_reply) is a pure
 * predicate, callable from tests without a provider or a turn —
 * see .claude/rules/security-predicate-extraction.md.
 */

#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_agent;

typedef enum hu_mod_ctx_verdict {
    HU_MOD_CTX_ERROR = 0, /* judge unavailable / timed out / unparseable: fail closed */
    HU_MOD_CTX_IDIOM,     /* idiom, hyperbole, sport, media, or a past event — not a threat */
    HU_MOD_CTX_REAL,      /* threatens, incites or endorses real-world violence against a person */
} hu_mod_ctx_verdict_t;

/* Gate mode from HU_MODERATION_CONTEXT; default OFF. */
hu_gate_mode_t hu_moderation_context_mode(void);

/* Pure decision table: should the reply that triggered a keyword violence
 * hit be KEPT (not replaced by the canned decline)? Only LIVE + IDIOM keeps
 * it; every other (mode, verdict) pair replaces, matching today's behaviour.
 * NULL-safe in the sense that there is nothing to dereference: enum inputs
 * only. */
bool hu_moderation_context_keep_reply(hu_gate_mode_t mode, hu_mod_ctx_verdict_t verdict);

/* One local-model call judging whether `reply` (the twin's own drafted
 * outbound message, NOT the inbound message) threatens real-world violence
 * against a person, or is idiom/hyperbole/sport/media/a past event. `local`'s
 * vtable NULL, or no chat_with_system, or a NULL/empty `reply` -> ERROR
 * without a call. Tags the thread X-HU-Purpose: moderation_check for the
 * duration of the call (restored afterwards). */
hu_mod_ctx_verdict_t hu_moderation_context_judge(const hu_provider_t *local, hu_allocator_t *alloc,
                                                 const char *model, size_t model_len,
                                                 const char *reply, size_t reply_len);

/* SHIELD-004 glue: runs hu_moderation_check on *response and, on any flagged
 * hit, replaces *response with the existing safe decline (hu_self_harm_
 * decline_or_floor) exactly as before — UNLESS HU_MODERATION_CONTEXT is LIVE,
 * the hit is violence-only (no hate), and the judge says IDIOM, in which case
 * *response is left untouched. `inbound` is the INBOUND message this turn is
 * replying to (used only by the existing decline/floor logic, never by the
 * judge). Logs the SHADOW/LIVE aggregate line via `agent`'s logger. Returns
 * true when *response was replaced. */
bool hu_moderation_shield_apply(struct hu_agent *agent, const char *inbound, size_t inbound_len,
                                char **response, size_t *response_len);

#ifdef HU_IS_TEST
/* hu_moderation_shield_apply's local-provider resolution, for hermetic
 * tests: NULL -> no local provider (ERROR verdict); non-NULL stands in for
 * hu_proposer_context_local_provider's real resolution of agent->provider. */
void hu_moderation_context_set_test_provider(const hu_provider_t *p);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_SECURITY_MODERATION_CONTEXT_H */
