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
 * (0 fires in prod). Their firing was also a static coin flip (2% for
 * self-reaction, persona.double_text_probability, persona.gif_probability).
 *
 * HU_SPONTANEITY=off|shadow|live (default off):
 *   off    — exactly today's code path, including the unreachable defer check.
 *   shadow — today's path, plus one aggregate log line per eligible extra:
 *            "[HU_SPONTANEITY shadow] kind=… eligible=1 rate_src=… p=… would_fire=…".
 *   live   — the defer check is replaced by a cap of ONE extra per reactive
 *            turn; an eligible extra fires with probability
 *              p = learned_rate × (0.8 + 0.45·θ)
 *            where learned_rate is Seth's own measured rate from
 *            <persona>.learned-style.json (double_text_rate, self_reaction_rate,
 *            gif_rate) and θ is a fresh Thompson draw from the contact's
 *            humanization-bandit arm (1.0 multiplier for a contact with no
 *            outcomes yet). No learned rate → the extra never fires LIVE; it
 *            is only logged as in shadow. Eligibility (no farewell, not 3 of
 *            our last 4 messages, no GIF on a question / sad news, rate cap)
 *            is unchanged from the legacy predicates. */

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
    HU_SPONT_KIND_COUNT
} hu_spontaneity_kind_t;

/* Seth's measured per-reply rates, [0,1]; < 0 = not measured. */
typedef struct hu_spontaneity_rates {
    double rate[HU_SPONT_KIND_COUNT];
} hu_spontaneity_rates_t;

typedef struct hu_spontaneity_decision {
    bool fire;
    bool have_rate; /* learned rate present */
    double p;       /* firing probability used (0 when no rate) */
} hu_spontaneity_decision_t;

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
    int64_t fallback_sent_id;      /* non-iMessage: our message id */
    const hu_provider_t *classify; /* fast provider for the afterthought, or NULL */
    const char *classify_model;
    size_t classify_model_len;
    unsigned fired; /* extras sent this turn (LIVE cap: 1) */
} hu_spontaneity_turn_t;

hu_gate_mode_t hu_spontaneity_mode(void);

/* Parse the optional rate fields of a learned-style JSON object. Missing or
 * out-of-range fields stay < 0. HU_ERR_PARSE when json is not an object. */
hu_error_t hu_spontaneity_rates_parse(hu_allocator_t *alloc, const char *json, size_t len,
                                      hu_spontaneity_rates_t *out);

/* The pure policy. learned_rate < 0 → never fires. theta < 0 → no bandit
 * residual (multiplier 1). u is a uniform draw in [0,1). */
hu_spontaneity_decision_t hu_spontaneity_policy(double learned_rate, double theta, double u);

/* Call sites (src/daemon.c reactive path, after the reply was sent). Each
 * evaluates its own legacy condition (including the defer check) so OFF is
 * the original code path. */
void hu_daemon_spontaneity_double_text(hu_spontaneity_turn_t *t);
void hu_daemon_spontaneity_self_reaction(hu_spontaneity_turn_t *t);
/* GIF, in two steps around the daemon's GIF block. gif_open replaces the
 * block's defer check (OFF/SHADOW: !defer — SHADOW also logs the would-fire
 * decision; LIVE: the per-turn cap). gif_roll replaces its probability roll
 * (OFF/SHADOW: should_send_gif at legacy_prob plus the rate cap; LIVE:
 * eligibility, rate cap, then the learned policy). */
bool hu_daemon_spontaneity_gif_open(hu_spontaneity_turn_t *t, uint64_t now_ms);
bool hu_daemon_spontaneity_gif_roll(hu_spontaneity_turn_t *t, uint32_t seed, float legacy_prob,
                                    uint64_t now_ms);

#if HU_IS_TEST
/* Force the gate (hu_gate_mode_t) and the rates / uniform draws in tests.
 * mode -1 = read env; rates NULL = read the persona file. */
void hu_spontaneity_set_for_test(int mode, const hu_spontaneity_rates_t *rates, uint32_t seed);
/* The LIVE/SHADOW decision for one extra, as the call sites use it. */
bool hu_spontaneity_decide_for_test(hu_spontaneity_turn_t *t, hu_spontaneity_kind_t kind,
                                    bool eligible);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_SPONTANEITY_H */
