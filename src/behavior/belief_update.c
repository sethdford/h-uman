/*
 * belief_update.c — pure conviction-loop decision faculty (A1).
 *
 * No SQLite, no agent state: see include/human/behavior/belief_update.h and
 * docs/plans/2026-05-29-conviction-loop/. The DB write + signal wiring live
 * in daemon.c; this file is only the decision + two pure helpers.
 */

#include "human/behavior/belief_update.h"

#include <ctype.h>
#include <string.h>

hu_belief_update_t hu_belief_update_decide(const hu_belief_facts_t *f) {
    if (!f)
        return HU_BELIEF_NO_CHANGE;

    /* Nothing to update, or nothing to update WITH. */
    if (!f->stance_exists || !f->has_new_evidence)
        return HU_BELIEF_NO_CHANGE;

    /* Anti-sycophancy spine (AC-2): a reassertion is repetition, not
     * persuasion. It VETOES any update regardless of other signals. */
    if (f->is_reassertion)
        return HU_BELIEF_NO_CHANGE;

    /* Per-conversation cap (AC-4): don't whiplash within one conversation. */
    if (f->changes_this_convo >= HU_BELIEF_MAX_CHANGES_PER_CONVO)
        return HU_BELIEF_NO_CHANGE;

    if (f->evidence_contradicts) {
        /* Strong convictions erode before they snap — flip only when the
         * held conviction is not strong. Ceiling is inclusive of "strong". */
        if (f->current_conviction >= HU_BELIEF_FLIP_CONVICTION_CEIL)
            return HU_BELIEF_WEAKEN;
        return HU_BELIEF_FLIP;
    }

    /* New evidence that agrees reinforces the held stance. */
    return HU_BELIEF_STRENGTHEN;
}

/* Word-boundary, case-insensitive substring match. A match counts only when
 * bounded by start/end of string or a non-alphanumeric char, so "fact" does
 * not match inside "factory" (see substring-classifier-pitfalls.md). The
 * needle may itself contain spaces (e.g. "turns out"); those are matched
 * literally and only the OUTER edges are boundary-checked. */
static bool contains_word_ci(const char *s, size_t slen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || slen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= slen; i++) {
        if (strncasecmp(s + i, needle, nlen) != 0)
            continue;
        bool left_ok = (i == 0) || !isalnum((unsigned char)s[i - 1]);
        bool right_ok = (i + nlen == slen) || !isalnum((unsigned char)s[i + nlen]);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

bool hu_belief_msg_has_evidence_cue(const char *msg, size_t len) {
    if (!msg || len == 0)
        return false;

    /* Markers that signal an argument or fact is being offered, rather than
     * a bare restatement of a position. Deliberately conservative: a true
     * cue should accompany a reason, not just emphasis. */
    static const char *const cues[] = {
        "because", "since",   "data",  "study",      "studies",  "research",  "evidence",
        "source",  "sources", "fact",  "facts",      "actually", "turns out", "found",
        "shows",   "showed",  "proof", "statistics", "cite",     "according",
    };
    const size_t n = sizeof(cues) / sizeof(cues[0]);
    for (size_t i = 0; i < n; i++) {
        if (contains_word_ci(msg, len, cues[i]))
            return true;
    }
    return false;
}

double hu_belief_conviction_for(hu_belief_update_t decision, double current) {
    switch (decision) {
    case HU_BELIEF_STRENGTHEN: {
        double v = current + 0.2;
        return v > 1.0 ? 1.0 : v;
    }
    case HU_BELIEF_WEAKEN: {
        double v = current - 0.2;
        return v < 0.0 ? 0.0 : v;
    }
    case HU_BELIEF_FLIP:
        return 0.55; /* fresh moderate conviction in the new direction */
    case HU_BELIEF_NO_CHANGE:
    default:
        return current;
    }
}

#ifdef HU_ENABLE_SQLITE

#include "human/behavior/dialog_act.h"
#include "human/humanness.h" /* hu_evolved_opinion_t */
#include "human/memory/evolved_opinions.h"

/* Longest user-message snippet stored as a flipped stance. */
#define HU_BELIEF_FLIP_STANCE_MAX 160

hu_error_t hu_belief_update_evaluate_turn(hu_allocator_t *alloc, sqlite3 *op_db,
                                          const hu_pressure_history_t *ph, const char *user_msg,
                                          size_t user_msg_len, uint32_t changes_this_convo,
                                          int64_t now_ts, char **out_directive,
                                          size_t *out_directive_len, bool *out_changed) {
    if (out_directive)
        *out_directive = NULL;
    if (out_directive_len)
        *out_directive_len = 0;
    if (out_changed)
        *out_changed = false;
    if (!alloc || !op_db || !user_msg || user_msg_len == 0)
        return HU_OK;

    /* Turn-level signals, computed once. */
    bool has_evidence = hu_belief_msg_has_evidence_cue(user_msg, user_msg_len);
    if (!has_evidence)
        return HU_OK; /* no argument offered — nothing to update on */

    bool contradicts = (hu_dialog_act_classify(user_msg, user_msg_len) == HU_DACT_DISAGREEMENT);

    bool reasserted = false;
    if (ph)
        hu_pressure_history_inspect(ph, user_msg, user_msg_len, &reasserted, NULL);

    /* Held opinions — consider all (min_conviction 0.0). */
    hu_evolved_opinion_t *ops = NULL;
    size_t n = 0;
    if (hu_evolved_opinions_get(alloc, op_db, 0.0, 16, &ops, &n) != HU_OK || !ops || n == 0)
        return HU_OK;

    hu_error_t rc = HU_OK;
    for (size_t i = 0; i < n; i++) {
        if (!ops[i].topic || ops[i].topic_len == 0)
            continue;
        /* Does the user message reference this topic? Word-boundary, ci. The
         * topic string from the DB is NUL-terminated. */
        if (!contains_word_ci(user_msg, user_msg_len, ops[i].topic))
            continue;

        hu_belief_facts_t f;
        f.stance_exists = true;
        f.has_new_evidence = has_evidence;
        f.is_reassertion = reasserted;
        f.evidence_contradicts = contradicts;
        f.current_conviction = ops[i].conviction;
        f.changes_this_convo = changes_this_convo;

        hu_belief_update_t d = hu_belief_update_decide(&f);
        if (d == HU_BELIEF_NO_CHANGE)
            break; /* matched the turn's topic; no update warranted */

        double new_conv = hu_belief_conviction_for(d, ops[i].conviction);

        /* STRENGTHEN/WEAKEN keep the stance (conviction-only); FLIP adopts the
         * user's competing claim (snippet) as the new stance. */
        const char *new_stance = ops[i].stance;
        size_t new_stance_len = ops[i].stance_len;
        char snippet[HU_BELIEF_FLIP_STANCE_MAX];
        if (d == HU_BELIEF_FLIP) {
            size_t cap = user_msg_len < HU_BELIEF_FLIP_STANCE_MAX - 1
                             ? user_msg_len
                             : HU_BELIEF_FLIP_STANCE_MAX - 1;
            for (size_t k = 0; k < cap; k++)
                snippet[k] =
                    (char)((user_msg[k] == '\n' || user_msg[k] == '\r') ? ' ' : user_msg[k]);
            snippet[cap] = '\0';
            new_stance = snippet;
            new_stance_len = cap;
        }

        const char *reason = d == HU_BELIEF_FLIP     ? "user presented contradicting evidence"
                             : d == HU_BELIEF_WEAKEN ? "user challenged this with evidence"
                                                     : "user reinforced this with evidence";

        size_t dir_len = 0;
        char *dir = hu_evolved_opinion_upsert_with_history(
            alloc, op_db, ops[i].topic, ops[i].topic_len, new_stance, new_stance_len, new_conv,
            now_ts, reason, strlen(reason), changes_this_convo, &dir_len);

        if (out_changed)
            *out_changed = true;
        if (out_directive)
            *out_directive = dir;
        else if (dir)
            alloc->free(alloc->ctx, dir, dir_len + 1);
        if (out_directive_len)
            *out_directive_len = dir_len;
        break; /* at most one belief update per turn */
    }

    hu_evolved_opinions_free(alloc, ops, n);
    return rc;
}

#endif /* HU_ENABLE_SQLITE */
