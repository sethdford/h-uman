#ifndef HU_DAEMON_SPONTANEITY_H
#define HU_DAEMON_SPONTANEITY_H

/* Spontaneous extras after a reactive reply — the double-text afterthought,
 * the self-reaction and the GIF — and the HU_SPONTANEITY gate that decides
 * when they fire (DEF-15, 2026-10-02).
 *
 * Why this exists: all three were gated on "did the reactive path send to
 * this contact in the last 60 s" (hu_daemon_proactive_should_defer), but the
 * reactive path records that send (src/daemon.c, FU-1) just BEFORE they are
 * checked, so the predicate was always true and none of them ever fired
 * (0 fires in prod). Their firing was also a static coin flip.
 *
 * HU_SPONTANEITY=off|shadow|live (default off):
 *   off    — exactly the previous code path, including the unreachable defer.
 *   shadow — the previous path, plus ONE aggregate line per reactive turn:
 *            "[HU_SPONTANEITY shadow] turn=1 dt=<e>/<p>/<f> sr=… gif=… chosen=<kind|none>"
 *            (e = eligible, p = sampled firing probability, f = would fire).
 *   live   — same line tagged "live" with fired=0|1. Per reactive turn at most
 *            ONE extra fires: each eligible kind draws p from its learned
 *            per-(contact, kind) Beta posterior and fires with probability p;
 *            among the kinds that fire, one is chosen uniformly at random.
 *            Nothing sleeps on the daemon loop: the afterthought goes through
 *            hu_conversation_schedule_message_on, the self-reaction and the
 *            GIF through this module's queue, drained by
 *            hu_daemon_spontaneity_tick.
 *
 * The learned posterior. Prior Beta(r·n0, (1−r)·n0) from Seth's own per-reply
 * rate r and its sample size n0 (learned-style.json, `global` block:
 * <kind>_rate and <kind>_n, n0 falling back to n_eff then n); outcomes add to
 * it: after an extra fires, the contact replying or tapping back (on a
 * message the DEF-8 join attributes to the daemon) is a success, silence for
 * HU_SPONT_OUTCOME_HORIZON_S a failure. Counts persist in
 * ~/.human/bandit_spontaneity.json. No rate in the file → that kind never
 * fires live (it is still logged). */

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_agent;
struct hu_config;

typedef enum hu_spontaneity_kind {
    HU_SPONT_DOUBLE_TEXT = 0,
    HU_SPONT_SELF_REACTION,
    HU_SPONT_GIF,
    HU_SPONT_KIND_COUNT,
    HU_SPONT_NONE = HU_SPONT_KIND_COUNT
} hu_spontaneity_kind_t;

/* Silence this long after an extra counts as a failure — the same horizon the
 * proactive outcome resolver uses for IGNORED (src/ml/dpo.c). */
#define HU_SPONT_OUTCOME_HORIZON_S 86400

/* Seth's measured per-reply rates, [0,1] (< 0 = not measured), and the number
 * of eligible replies each was measured on (<= 0 = unknown). */
typedef struct hu_spontaneity_rates {
    double rate[HU_SPONT_KIND_COUNT];
    double n0[HU_SPONT_KIND_COUNT];
    double double_text_gap_s; /* Seth's median afterthought gap; < 0 = not measured */
} hu_spontaneity_rates_t;

/* Everything one reactive turn's extras need; filled once at the call site. */
typedef struct hu_spontaneity_turn {
    struct hu_agent *agent;
    const struct hu_config *config;
    hu_channel_t *channel;
    hu_allocator_t *alloc;
    const char *contact; /* batch key (handle) */
    size_t contact_len;
    const char *send_target;
    size_t send_target_len;
    const char *response; /* what we just sent */
    size_t response_len;
    const char *inbound; /* what they sent */
    size_t inbound_len;
    const hu_channel_history_entry_t *history;
    size_t history_count;
    uint8_t hour_local;
    bool is_group;
    bool gif_available;            /* force GIF availability (tests); else read from config */
    int64_t fallback_sent_id;      /* non-iMessage: our message id */
    const hu_provider_t *classify; /* fast provider for the afterthought, or NULL */
    const char *classify_model;
    size_t classify_model_len;
    hu_spontaneity_kind_t chosen; /* set by hu_daemon_spontaneity_choose */
} hu_spontaneity_turn_t;

hu_gate_mode_t hu_spontaneity_mode(void);

/* Parse the optional rate fields of a learned-style JSON object (`global`
 * block first, then top level). HU_ERR_PARSE when json is not an object. */
hu_error_t hu_spontaneity_rates_parse(hu_allocator_t *alloc, const char *json, size_t len,
                                      hu_spontaneity_rates_t *out);

/* One Thompson draw of the firing probability for (contact, kind): Beta with
 * the learned prior plus the contact's outcome counts. Returns < 0 when the
 * kind has no learned rate (never fires live). */
double hu_spontaneity_sample_p(const char *contact, size_t contact_len, hu_spontaneity_kind_t kind,
                               const hu_spontaneity_rates_t *rates);

/* Per reactive turn, before the extras: SHADOW/LIVE decide which ONE extra (if
 * any) fires and log the turn line; sets and returns t->chosen. OFF: NONE. */
hu_spontaneity_kind_t hu_daemon_spontaneity_choose(hu_spontaneity_turn_t *t);

/* Call sites (src/daemon.c reactive path, after the reply was sent). OFF and
 * SHADOW run the original code; LIVE acts only for t->chosen, without sleeping. */
void hu_daemon_spontaneity_double_text(hu_spontaneity_turn_t *t);
void hu_daemon_spontaneity_self_reaction(hu_spontaneity_turn_t *t);
/* GIF: gif_open replaces the GIF block's defer check, gif_roll its
 * probability roll, gif_send its sleep+send. gif_send takes ownership of
 * gif_path (alloc'd with t->alloc) and returns true when the GIF went out
 * (OFF/SHADOW) or was queued (LIVE). */
bool hu_daemon_spontaneity_gif_open(hu_spontaneity_turn_t *t, uint64_t now_ms);
bool hu_daemon_spontaneity_gif_roll(hu_spontaneity_turn_t *t, uint32_t seed, float legacy_prob,
                                    uint64_t now_ms);
bool hu_daemon_spontaneity_gif_send(hu_spontaneity_turn_t *t, char *gif_path, const char *query,
                                    size_t query_len, uint32_t seed, uint64_t now_ms);

/* Daemon pass: deliver queued self-reactions / GIFs that are due on the
 * matching channel, and resolve expired outcomes as failures. */
void hu_daemon_spontaneity_tick(hu_channel_t *channel, const char *channel_name, int64_t now_ms);

/* Outcome feedback. on_inbound: the contact wrote to us. on_engagement: the
 * contact tapped back on a message the DEF-8 join attributes to the daemon,
 * sent at sent_ms. Both credit a pending extra delivered before them. */
void hu_daemon_spontaneity_on_inbound(const char *contact, size_t contact_len, int64_t now_ms);
void hu_daemon_spontaneity_on_engagement(const char *contact, size_t contact_len, int64_t sent_ms);

#if HU_IS_TEST
/* Force the gate (hu_gate_mode_t), rates and RNG seed in tests. mode -1 = read
 * env; rates NULL = read the persona file. Also clears queues and outcomes. */
void hu_spontaneity_set_for_test(int mode, const hu_spontaneity_rates_t *rates, uint32_t seed);
/* Outcome counts (successes, failures) recorded for (contact, kind). */
void hu_spontaneity_outcomes_for_test(const char *contact, hu_spontaneity_kind_t kind,
                                      uint64_t *succ, uint64_t *fail);
/* Queued (not yet delivered) self-reactions + GIFs. */
size_t hu_spontaneity_queued_for_test(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_SPONTANEITY_H */
