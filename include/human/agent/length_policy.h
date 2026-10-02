#ifndef HU_AGENT_LENGTH_POLICY_H
#define HU_AGENT_LENGTH_POLICY_H

/* Reply-length policy: HU_LENGTH_POLICY=off|shadow|live, default off.
 *
 * Why (2026-10-01 humanness audit): the median reply was 22 chars against the
 * owner's ~71. The 1:1 cap was a multiple of the INBOUND length (2.0x, up to
 * 3.25x for a deep relationship, 15-char floor, floored at the contact's
 * measured p90), and the prompt said "Keep it tight" for every cap <= 80 —
 * so a contact whose measured p90 is 50 was told to keep it tight every turn,
 * and the quality retry asked for "Tighten up significantly" whenever a reply
 * ran past 5x their last message, even inside the cap.
 *
 *   OFF     today's cap and prompt wording, byte for byte.
 *   SHADOW  computes the new cap and logs one aggregate line per 1:1 turn
 *           ("[HU_LENGTH_POLICY shadow] old_cap=… new_cap=… tight_old=…
 *           tight_new=…"); applies today's cap.
 *   LIVE    for a 1:1 contact with measured reply stats: cap = the owner's
 *           own p90 to that contact, raised (never lowered) for a long,
 *           story- or question-shaped inbound, never below the owner's p50,
 *           never above the hard bound. The prompt says "keep it tight" only
 *           when the cap is below the p50, and the quality retry scores
 *           against the same cap. Groups and contacts without stats keep
 *           today's cap and wording exactly.
 *
 * Promotion (SHADOW→LIVE) and rollback: docs/guides/length-policy.md. */

#include "human/core/gate_mode.h"
#include "human/persona/relationship.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_contact_profile;

/* Absolute ceiling when the channel sets none. 600 is where the prompt's
 * texting shape rules stop applying (prompt.c append_texting_shape_rules). */
#define HU_LENGTH_POLICY_HARD_MAX 600u

/* An inbound this long (bytes) is story-shaped whatever its punctuation. */
#define HU_LENGTH_POLICY_LONG_INBOUND 120u

/* p50 derived from p90 when only p90 is measured. Measured 2026-10-01 with
 * scripts/measure_contact_reply_lengths.py's measure() over 60 days (owner-
 * authored sends only, n >= 20; aggregate only): 6 contacts, p50/p90 median
 * 0.37, range 0.16–0.53. 1/3 sits just under the median, so the floor rarely
 * exceeds the true p50 and the cap never grows from a guessed median. */
#define HU_LENGTH_POLICY_P50_NUM 1u
#define HU_LENGTH_POLICY_P50_DEN 3u

/* Today's prompt rule: a cap of 1..80 says "Keep it tight". */
#define HU_LENGTH_POLICY_LEGACY_TIGHT_MAX 80u

enum {
    HU_LENGTH_SHAPE_QUESTION = 1u << 0, /* contains '?' */
    HU_LENGTH_SHAPE_STORY = 1u << 1,    /* long, or several sentences / lines */
};

/* What the RESPONSE LIMIT line says. LEGACY (0, the zero-initialized default)
 * keeps today's rule; LIVE sets NO/YES from cap < p50. */
typedef enum hu_length_tight {
    HU_LENGTH_TIGHT_LEGACY = 0,
    HU_LENGTH_TIGHT_NO,
    HU_LENGTH_TIGHT_YES,
} hu_length_tight_t;

typedef struct hu_length_policy_input {
    size_t inbound_len;
    unsigned shape;       /* HU_LENGTH_SHAPE_* */
    uint32_t contact_p50; /* owner's median reply to this contact; 0 = unmeasured */
    uint32_t contact_p90; /* owner's p90 reply to this contact; 0 = unmeasured */
    hu_relationship_stage_t stage;
    uint32_t legacy_cap; /* today's cap; returned verbatim without stats */
    uint32_t hard_max;   /* channel / config bound; 0 = HU_LENGTH_POLICY_HARD_MAX */
    uint32_t brief_cap;  /* brief-mode cap; 0 = not a brief turn */
} hu_length_policy_input_t;

typedef struct hu_length_policy_result {
    uint32_t cap;
    bool tight;       /* the prompt should say "keep it tight" */
    uint32_t p50;     /* the p50 used (measured or derived); 0 without stats */
    bool p50_derived; /* p50 came from p90 * NUM / DEN */
    bool from_stats;  /* false: legacy_cap returned verbatim */
} hu_length_policy_result_t;

/* Pure. Without contact stats returns {legacy_cap, legacy tight rule}. */
hu_length_policy_result_t hu_length_policy_compute(const hu_length_policy_input_t *in);

/* Pure. HU_LENGTH_SHAPE_* flags for an inbound message. */
unsigned hu_length_policy_inbound_shape(const char *inbound, size_t len);

/* Today's prompt rule: 0 < cap <= 80. */
bool hu_length_policy_legacy_tight(uint32_t cap);

/* One reactive turn's length decision: today's daemon F15 calibration and
 * brief cap (moved here from daemon.c), then the policy per `mode`. */
typedef struct hu_length_turn {
    const char *inbound;
    size_t inbound_len;
    const struct hu_contact_profile *contact; /* NULL in a group / unknown contact */
    hu_relationship_stage_t stage;
    uint32_t channel_max; /* channel response constraint; 0 = none */
    bool is_group;
    bool brief_mode;
    bool quiet; /* skip the SHADOW/LIVE log line (a second read of the same turn) */
} hu_length_turn_t;

typedef struct hu_length_turn_result {
    uint32_t cap;            /* the cap this turn applies */
    hu_length_tight_t tight; /* LEGACY unless LIVE with stats */
    uint32_t old_cap;        /* today's cap */
    uint32_t new_cap;        /* the policy's cap (== old_cap without stats) */
    bool from_stats;
} hu_length_turn_result_t;

/* Fills *out. SHADOW and LIVE log one aggregate line per 1:1 turn (caps,
 * flags, byte length, enums — never text or a contact key). */
void hu_length_policy_turn(const hu_length_turn_t *t, hu_gate_mode_t mode,
                           hu_length_turn_result_t *out);

/* The reference length the quality scorer's OVER-length checks divide by.
 * LIVE with a cap: max(ref_len, ceil(max_chars / 1.5)), so a reply inside the
 * cap keeps full brevity marks and never trips the 5x "tighten up" retry.
 * Otherwise ref_len unchanged. */
size_t hu_length_policy_quality_over_ref(size_t ref_len, uint32_t max_chars, hu_gate_mode_t mode);

/* HU_LENGTH_POLICY, default OFF (unknown values fail closed). */
hu_gate_mode_t hu_length_policy_mode(void);

/* Test hook: force a mode (HU_GATE_*), or -1 to read the env again. */
void hu_length_policy_set_mode_for_test(int mode);

#endif /* HU_AGENT_LENGTH_POLICY_H */
