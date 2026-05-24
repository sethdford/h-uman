/* Spec 2026-05-24-action-layers — drift + clarification directive emitters.
 *
 * Stateless readers over Spec 3's agent_self_concerns and Spec 4's
 * tom_user_expectations. Called by the daemon at pre-turn assembly to
 * append short directives to the system prompt. See header for contract.
 *
 * Feature-flag (AC-AL-3): when HU_ENABLE_ACTION_LAYERS is NOT defined,
 * both functions are stubs returning 0. Callers compile in both modes
 * without #ifdef at the call site. */

#include "human/agent/action_directives.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_ACTION_LAYERS

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

/* Translate the concern's dimension into a human-readable suggestion.
 * Privacy: dimension name only, never content. */
static const char *opposite_for_dimension(const char *dimension, double sigma) {
    if (!dimension)
        return "moderate";
    if (strcmp(dimension, "response_length") == 0)
        return sigma > 0 ? "shorter replies" : "more detailed replies";
    if (strcmp(dimension, "tool_entropy") == 0)
        return sigma > 0 ? "fewer tool calls" : "leaning on tools more";
    if (strcmp(dimension, "emotion_consistency") == 0)
        return "matching the contact's register";
    return "balanced";
}

size_t hu_action_directive_drift(struct sqlite3 *db, char *out, size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
#ifdef HU_ENABLE_SQLITE
    if (!db)
        return 0;
    /* Most-recent unresolved concern within last 7 days at >= 2.0 sigma.
     * AC-AL-1 + D-AL-3 (one concern per turn, most recent). */
    static const char *sql =
        "SELECT dimension, magnitude_sigma, window_n_turns "
        "FROM agent_self_concerns "
        "WHERE ABS(magnitude_sigma) >= 2.0 "
        "  AND created_ts_ms >= ((CAST(strftime('%s','now') AS INTEGER) - 7*86400) * 1000) "
        "ORDER BY created_ts_ms DESC LIMIT 1";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        if (st)
            sqlite3_finalize(st);
        return 0;
    }
    size_t written = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *dim = (const char *)sqlite3_column_text(st, 0);
        double sigma = sqlite3_column_double(st, 1);
        int n_turns = sqlite3_column_int(st, 2);
        const char *suggestion = opposite_for_dimension(dim, sigma);
        int n = snprintf(out, out_cap,
                         "### Self-observation drift\nRecent: %s is %+.1fσ from baseline "
                         "(window=%d turns). Lean toward %s in this turn.\n",
                         dim ? dim : "unknown", sigma, n_turns, suggestion);
        if (n > 0)
            written = (size_t)n < out_cap ? (size_t)n : out_cap - 1;
    }
    sqlite3_finalize(st);
    return written;
#else
    (void)db;
    return 0;
#endif
}

/* Map the expected_knowledge_type enum int to a short word.
 * Mirrors hu_tom_expectation_type_t from include/human/agent/theory_of_mind.h
 * — keep in sync. */
static const char *kind_label(int knowledge_type) {
    switch (knowledge_type) {
    case 0:
        return "remembers";
    case 1:
        return "understands";
    case 2:
        return "tracks";
    default:
        return "knows";
    }
}

size_t hu_action_directive_clarify(struct sqlite3 *db, const char *contact_id,
                                   size_t contact_id_len, const char *current_session_key,
                                   size_t current_session_key_len, int64_t now_unix_ms, char *out,
                                   size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
#ifdef HU_ENABLE_SQLITE
    if (!db || !contact_id || contact_id_len == 0)
        return 0;
    /* "Stale enough" per D-AL-4: created in a session_key DIFFERENT from
     * the current one OR ≥10 min ago. We OR them: either condition
     * qualifies. NULL session_key on the row matches "different from
     * current" trivially. */
    static const char *sql = "SELECT topic, expected_knowledge_type "
                             "FROM tom_user_expectations "
                             "WHERE contact_id = ? "
                             "  AND resolved_ts_ms IS NULL "
                             "  AND (session_key IS NULL OR session_key != ? "
                             "       OR created_ts_ms <= ? - (10 * 60 * 1000)) "
                             "ORDER BY created_ts_ms DESC LIMIT 1";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        if (st)
            sqlite3_finalize(st);
        return 0;
    }
    sqlite3_bind_text(st, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    if (current_session_key && current_session_key_len > 0) {
        sqlite3_bind_text(st, 2, current_session_key, (int)current_session_key_len, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(st, 2);
    }
    sqlite3_bind_int64(st, 3, now_unix_ms);
    size_t written = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *topic = (const char *)sqlite3_column_text(st, 0);
        int ktype = sqlite3_column_int(st, 1);
        int n = snprintf(out, out_cap,
                         "### Unmet expectation needing clarification\n"
                         "User %s about \"%s\" — no recorded belief. Consider asking "
                         "briefly rather than guessing.\n",
                         kind_label(ktype), topic ? topic : "(topic)");
        if (n > 0)
            written = (size_t)n < out_cap ? (size_t)n : out_cap - 1;
    }
    sqlite3_finalize(st);
    return written;
#else
    (void)db;
    (void)contact_id;
    (void)contact_id_len;
    (void)current_session_key;
    (void)current_session_key_len;
    (void)now_unix_ms;
    return 0;
#endif
}

#else /* !HU_ENABLE_ACTION_LAYERS */

/* Stubs: when the flag is OFF, no directive is ever emitted. Callers
 * compile and link cleanly. */
size_t hu_action_directive_drift(struct sqlite3 *db, char *out, size_t out_cap) {
    (void)db;
    if (out && out_cap > 0)
        out[0] = '\0';
    return 0;
}

size_t hu_action_directive_clarify(struct sqlite3 *db, const char *contact_id,
                                   size_t contact_id_len, const char *current_session_key,
                                   size_t current_session_key_len, int64_t now_unix_ms, char *out,
                                   size_t out_cap) {
    (void)db;
    (void)contact_id;
    (void)contact_id_len;
    (void)current_session_key;
    (void)current_session_key_len;
    (void)now_unix_ms;
    if (out && out_cap > 0)
        out[0] = '\0';
    return 0;
}

#endif /* HU_ENABLE_ACTION_LAYERS */
