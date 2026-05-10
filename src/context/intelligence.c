#include "human/context/intelligence.h"
#include "human/core/string.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HU_INTELLIGENCE_ESCAPE_BUF 512

/* ── SQL escape ────────────────────────────────────────────────────────── */

static size_t escape_sql_string(const char *s, size_t len, char *out, size_t out_cap) {
    size_t n = 0;
    (void)hu_sql_quote_escape_into(s, len, out, out_cap, &n);
    return n;
}

/* ── F68 Protective Intelligence ─────────────────────────────────────────── */

hu_error_t hu_protective_create_table_sql(char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS boundaries (\n"
        "    id INTEGER PRIMARY KEY,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    topic TEXT NOT NULL,\n"
        "    type TEXT NOT NULL,\n"
        "    set_at INTEGER NOT NULL,\n"
        "    source TEXT\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_protective_insert_sql(const hu_boundary_t *b, char *buf, size_t cap, size_t *out_len)
{
    if (!b || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!b->contact_id || !b->topic || !b->type)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_INTELLIGENCE_ESCAPE_BUF];
    char topic_esc[HU_INTELLIGENCE_ESCAPE_BUF];
    char type_esc[HU_INTELLIGENCE_ESCAPE_BUF];

    size_t ce_len = escape_sql_string(b->contact_id, b->contact_id_len, contact_esc,
                                       sizeof(contact_esc));
    size_t te_len = escape_sql_string(b->topic, b->topic_len, topic_esc, sizeof(topic_esc));
    size_t ty_len = escape_sql_string(b->type, b->type_len, type_esc, sizeof(type_esc));

    if (ce_len == 0 && b->contact_id_len > 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (te_len == 0 && b->topic_len > 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (ty_len == 0 && b->type_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "INSERT INTO boundaries (contact_id, topic, type, set_at, source) "
                     "VALUES ('%s', '%s', '%s', %llu, 'user')",
                     contact_esc, topic_esc, type_esc, (unsigned long long)b->set_at);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_protective_query_sql(const char *contact_id, size_t len, char *buf, size_t cap,
                                   size_t *out_len)
{
    if (!contact_id || len == 0 || !buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_INTELLIGENCE_ESCAPE_BUF];
    size_t ce_len = escape_sql_string(contact_id, len, contact_esc, sizeof(contact_esc));
    if (ce_len == 0 && len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "SELECT id, contact_id, topic, type, set_at, source FROM boundaries "
                     "WHERE contact_id = '%s'",
                     contact_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

static bool topic_match_case_insensitive(const char *haystack, size_t hay_len,
                                         const char *needle, size_t needle_len)
{
    if (needle_len == 0 || hay_len < needle_len)
        return false;
    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        bool match = true;
        for (size_t j = 0; j < needle_len && match; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j]))
                match = false;
        }
        if (match)
            return true;
    }
    return false;
}

bool hu_protective_topic_is_blocked(const hu_boundary_t *boundaries, size_t count,
                                    const char *topic, size_t topic_len)
{
    if (!boundaries || !topic || topic_len == 0)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (!boundaries[i].topic || boundaries[i].topic_len == 0)
            continue;
        if (topic_match_case_insensitive(topic, topic_len, boundaries[i].topic,
                                         boundaries[i].topic_len))
            return true;
    }
    return false;
}

hu_error_t hu_protective_build_prompt(hu_allocator_t *alloc, const hu_boundary_t *boundaries,
                                      size_t count, char **out, size_t *out_len)
{
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (count == 0 || !boundaries)
        return HU_OK;

    /* Group by type: avoid, redirect, lie */
    char avoid_buf[2048] = {0};
    char redirect_buf[2048] = {0};
    char lie_buf[2048] = {0};
    size_t avoid_len = 0, redirect_len = 0, lie_len = 0;

    for (size_t i = 0; i < count; i++) {
        const hu_boundary_t *b = &boundaries[i];
        if (!b->topic || b->topic_len == 0 || !b->type)
            continue;
        if (b->type_len >= 6 && strncmp(b->type, "avoid", 5) == 0) {
            if (avoid_len > 0)
                avoid_len = hu_buf_appendf(avoid_buf, sizeof(avoid_buf), avoid_len, ", ");
            avoid_len = hu_buf_appendf(avoid_buf, sizeof(avoid_buf), avoid_len, "%.*s",
                                       (int)b->topic_len, b->topic);
        } else if (b->type_len >= 8 && strncmp(b->type, "redirect", 8) == 0) {
            if (redirect_len > 0)
                redirect_len = hu_buf_appendf(redirect_buf, sizeof(redirect_buf), redirect_len, ", ");
            redirect_len = hu_buf_appendf(redirect_buf, sizeof(redirect_buf), redirect_len, "%.*s",
                                        (int)b->topic_len, b->topic);
        } else if (b->type_len >= 3 && strncmp(b->type, "lie", 3) == 0) {
            if (lie_len > 0)
                lie_len = hu_buf_appendf(lie_buf, sizeof(lie_buf), lie_len, ", ");
            lie_len = hu_buf_appendf(lie_buf, sizeof(lie_buf), lie_len, "%.*s",
                                     (int)b->topic_len, b->topic);
        }
    }

    if (avoid_len == 0 && redirect_len == 0 && lie_len == 0)
        return HU_OK;

    size_t total = 32;
    if (avoid_len > 0)
        total += 10 + avoid_len;
    if (redirect_len > 0)
        total += 12 + redirect_len;
    if (lie_len > 0)
        total += 10 + lie_len;

    char *result = alloc->alloc(alloc->ctx, total);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    pos = hu_buf_appendf(result, total, pos, "[BOUNDARIES with this contact]: ");
    if (avoid_len > 0)
        pos = hu_buf_appendf(result, total, pos, "AVOID: %s. ", avoid_buf);
    if (redirect_len > 0)
        pos = hu_buf_appendf(result, total, pos, "REDIRECT: %s. ", redirect_buf);
    if (lie_len > 0)
        pos = hu_buf_appendf(result, total, pos, "LIE: %s. ", lie_buf);

    *out = result;
    *out_len = pos;
    return HU_OK;
}

void hu_boundary_deinit(hu_allocator_t *alloc, hu_boundary_t *b)
{
    if (!b || !alloc)
        return;
    if (b->contact_id) {
        alloc->free(alloc->ctx, b->contact_id, b->contact_id_len + 1);
        b->contact_id = NULL;
        b->contact_id_len = 0;
    }
    if (b->topic) {
        alloc->free(alloc->ctx, b->topic, b->topic_len + 1);
        b->topic = NULL;
        b->topic_len = 0;
    }
    if (b->type) {
        alloc->free(alloc->ctx, b->type, b->type_len + 1);
        b->type = NULL;
        b->type_len = 0;
    }
}

/* ── F69 Humor Generation ────────────────────────────────────────────────── */

/* LCG: state = (a * state + c) mod m. Returns next value in [0, 1). */
static double lcg_next(uint32_t *state)
{
    *state = (uint32_t)((1103515245 * (uint64_t)*state + 12345) % 2147483648u);
    return (double)*state / 2147483648.0;
}

hu_humor_style_t hu_humor_select_style(double closeness, bool serious_topic, bool in_crisis,
                                       const hu_humor_config_t *config, uint32_t seed)
{
    hu_humor_config_t cfg;
    if (config) {
        cfg = *config;
    } else {
        cfg.humor_probability = 0.2;
        cfg.never_during_crisis = true;
        cfg.never_during_serious = true;
        cfg.preferred = HU_HUMOR_STYLE_OBSERVATIONAL;
    }

    if (in_crisis && cfg.never_during_crisis)
        return HU_HUMOR_STYLE_NONE;
    if (serious_topic && cfg.never_during_serious)
        return HU_HUMOR_STYLE_NONE;

    uint32_t s = seed;
    if (lcg_next(&s) >= cfg.humor_probability)
        return HU_HUMOR_STYLE_NONE;

    /* Close: SELF_DEPRECATING/CALLBACK, distant: OBSERVATIONAL/DEADPAN */
    double r = lcg_next(&s);
    if (closeness >= 0.6) {
        return (r < 0.5) ? HU_HUMOR_STYLE_SELF_DEPRECATING : HU_HUMOR_STYLE_CALLBACK;
    }
    return (r < 0.5) ? HU_HUMOR_STYLE_OBSERVATIONAL : HU_HUMOR_STYLE_DEADPAN;
}

const char *hu_humor_style_str(hu_humor_style_t style)
{
    switch (style) {
    case HU_HUMOR_STYLE_NONE:
        return "none";
    case HU_HUMOR_STYLE_CALLBACK:
        return "callback";
    case HU_HUMOR_STYLE_OBSERVATIONAL:
        return "observational";
    case HU_HUMOR_STYLE_SELF_DEPRECATING:
        return "self_deprecating";
    case HU_HUMOR_STYLE_ABSURD:
        return "absurd";
    case HU_HUMOR_STYLE_DEADPAN:
        return "deadpan";
    default:
        return "none";
    }
}

hu_error_t hu_humor_build_directive(hu_allocator_t *alloc, hu_humor_style_t style, char **out,
                                    size_t *out_len)
{
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (style == HU_HUMOR_STYLE_NONE)
        return HU_OK;

    const char *directive = NULL;
    switch (style) {
    case HU_HUMOR_STYLE_OBSERVATIONAL:
        directive = "[HUMOR STYLE: observational] — Notice something funny about the situation. "
                    "Keep it subtle.";
        break;
    case HU_HUMOR_STYLE_CALLBACK:
        directive = "[HUMOR STYLE: callback] — Reference a previous funny moment if relevant. "
                    "Keep it subtle.";
        break;
    case HU_HUMOR_STYLE_SELF_DEPRECATING:
        directive = "[HUMOR STYLE: self_deprecating] — Make light fun of yourself if appropriate. "
                    "Keep it subtle.";
        break;
    case HU_HUMOR_STYLE_ABSURD:
        directive = "[HUMOR STYLE: absurd] — Use unexpected or surreal humor if it fits. "
                    "Keep it subtle.";
        break;
    case HU_HUMOR_STYLE_DEADPAN:
        directive = "[HUMOR STYLE: deadpan] — Use dry, understated humor. Keep it subtle.";
        break;
    default:
        return HU_OK;
    }

    size_t len = strlen(directive);
    char *result = hu_strndup(alloc, directive, len);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    *out = result;
    *out_len = len;
    return HU_OK;
}

/* ── F102 Cognitive Load ────────────────────────────────────────────────── */

double hu_cognitive_compute_load(uint32_t active_convos, uint32_t msgs_this_hour,
                                  bool complex_topic)
{
    double load = (double)active_convos * 0.2 + (double)msgs_this_hour * 0.01 +
                  (complex_topic ? 0.3 : 0.0);
    if (load < 0.0)
        return 0.0;
    if (load > 1.0)
        return 1.0;
    return load;
}

hu_error_t hu_cognitive_build_directive(hu_allocator_t *alloc, const hu_cognitive_state_t *state,
                                        char **out, size_t *out_len)
{
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!state)
        return HU_ERR_INVALID_ARGUMENT;

    if (state->load_score < 0.3)
        return HU_OK;

    if (state->load_score <= 0.7)
        return HU_OK;

    /* load > 0.7: emit directive */
    static const char directive[] =
        "[COGNITIVE LOAD: high] Responses may be shorter. Focus on one conversation at a time.";
    size_t len = sizeof(directive) - 1;
    char *result = hu_strndup(alloc, directive, len);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    *out = result;
    *out_len = len;
    return HU_OK;
}
