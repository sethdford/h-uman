/* src/persona/social_insights.c
 *
 * Sprint A.5 wiring: turn the calibrate reaction-signature into a
 * prompt-ready paragraph. See header for the contract. */

#include "human/persona/social_insights.h"

#include "human/calibration.h"
#include "human/memory/personal_model.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

size_t hu_persona_render_social_insights(const struct hu_personal_model *model, char *out,
                                         size_t cap) {
    if (!model || !out || cap < 32)
        return 0;
    out[0] = '\0';

    hu_calib_reaction_signature_t sig;
    memset(&sig, 0, sizeof(sig));
    size_t reactor_count = hu_calib_reaction_signature_from_model(model, &sig);
    (void)reactor_count; /* sig.reactor_count is the source of truth */
    if (sig.reactor_count == 0 && sig.salient_topic_count == 0)
        return 0;

    size_t pos = 0;
    int n = snprintf(out + pos, cap - pos, "Reaction-derived insights:");
    if (n < 0 || (size_t)n >= cap - pos) {
        out[0] = '\0';
        return 0;
    }
    pos += (size_t)n;

    /* Top reactors, one per line. We cap at 5 in the rendered output
     * (the signature struct holds up to 8); rendering all 8 bloats the
     * prompt context for marginal value. */
    size_t reactors_to_render = sig.reactor_count > 5 ? 5 : sig.reactor_count;
    for (size_t i = 0; i < reactors_to_render && pos + 16 < cap; i++) {
        const hu_calib_top_reactor_t *r = &sig.top_reactors[i];
        n = snprintf(out + pos, cap - pos, "\n- %s: %u positive / %u negative recent reactions",
                     r->handle, r->positive_count, r->negative_count);
        if (n < 0 || (size_t)n >= cap - pos)
            break;
        pos += (size_t)n;
    }

    /* Salient topics on a single line, comma-separated. */
    if (sig.salient_topic_count > 0 && pos + 32 < cap) {
        n = snprintf(out + pos, cap - pos, "\nSalient topics from reactions:");
        if (n > 0 && (size_t)n < cap - pos)
            pos += (size_t)n;
        size_t topics_to_render = sig.salient_topic_count > 8 ? 8 : sig.salient_topic_count;
        for (size_t i = 0; i < topics_to_render && pos + 4 < cap; i++) {
            const char *sep = (i == 0) ? " " : ", ";
            n = snprintf(out + pos, cap - pos, "%s%s", sep, sig.salient_topics[i]);
            if (n < 0 || (size_t)n >= cap - pos)
                break;
            pos += (size_t)n;
        }
    }

    if (pos < cap)
        out[pos] = '\0';
    return pos;
}

/* ── Sprint A.7 — social_state.json consumer ─────────────────────────
 *
 * Reads the JSON snapshot written by hu_daemon_social_tick (every 6h
 * in production) and renders a prompt-friendly paragraph. Format
 * pinned by tests:
 *
 *   Recent social signals:
 *   - Alice has been quiet for 21 days (31 historical messages)
 *   - Bob's reply pattern has drifted (latency dimension, severity high)
 *
 * Tolerant by design: missing file, malformed JSON, empty arrays all
 * return 0. The consumer is best-effort; the persona prompt path must
 * NOT fail because the snapshot doesn't exist yet. */

#include <stdlib.h> /* getenv */

static const char *default_snapshot_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    int n = snprintf(buf, cap, "%s/.human/social_state.json", home);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}

/* Tiny JSON probe — find a "key": pattern in a small flat buffer and
 * return a pointer into `body` just past the colon, or NULL. Designed
 * for the specific shape hu_daemon_social_tick writes; not a general
 * JSON parser. */
static const char *find_key(const char *body, const char *key) {
    if (!body || !key)
        return NULL;
    /* Look for "<key>": */
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return NULL;
    const char *hit = strstr(body, needle);
    return hit ? (hit + n) : NULL;
}

/* Extract a string field from a flat JSON object substring. Writes
 * value into `out` (NUL-terminated, truncated to out_cap). Returns
 * bytes written, 0 on miss/malformed. */
static size_t extract_str_field(const char *obj, const char *key, char *out, size_t out_cap) {
    if (!obj || !out || out_cap < 2)
        return 0;
    out[0] = '\0';
    const char *p = find_key(obj, key);
    if (!p)
        return 0;
    /* Skip whitespace, expect opening quote */
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return 0;
    p++;
    size_t written = 0;
    while (*p && *p != '"' && written + 1 < out_cap) {
        out[written++] = *p++;
    }
    out[written] = '\0';
    return written;
}

/* Extract an integer field. Returns 0 on miss (caller distinguishes
 * by checking the field's presence via find_key first if needed). */
static long long extract_int_field(const char *obj, const char *key) {
    if (!obj)
        return 0;
    const char *p = find_key(obj, key);
    if (!p)
        return 0;
    return strtoll(p, NULL, 10);
}

size_t hu_persona_render_social_state_snapshot(const char *path, char *out, size_t cap) {
    if (!out || cap < 32)
        return 0;
    out[0] = '\0';

    char path_buf[512];
    const char *snap_path = path;
    if (!snap_path) {
        snap_path = default_snapshot_path(path_buf, sizeof(path_buf));
        if (!snap_path)
            return 0;
    }

    FILE *f = fopen(snap_path, "r");
    if (!f)
        return 0;
    /* Read up to 16 KB — snapshots are bounded by hu_daemon_social_tick's
     * stack buffer (8 KB), so 16 KB is plenty of headroom. */
    char body[16384];
    size_t br = fread(body, 1, sizeof(body) - 1, f);
    fclose(f);
    if (br == 0)
        return 0;
    body[br] = '\0';

    size_t pos = 0;
    int n;
    bool any = false;

    /* Stale contacts — render top 3. Each entry has {"contact":"...",
     * "days_since_last":<int>, "historical_count":<int>}. We walk the
     * array by looking for "contact":" tokens within the stale_contacts
     * subobject. */
    const char *stale_start = strstr(body, "\"stale_contacts\":");
    const char *sigs_start = strstr(body, "\"signatures\":");
    if (stale_start && sigs_start && stale_start < sigs_start) {
        size_t rendered = 0;
        const char *cursor = stale_start;
        while (rendered < 3 && cursor < sigs_start) {
            const char *contact_key = strstr(cursor, "\"contact\":\"");
            if (!contact_key || contact_key >= sigs_start)
                break;
            /* Treat the immediate region around contact_key as an
             * object substring — extract its three fields. */
            char handle[64];
            size_t handle_n = extract_str_field(contact_key, "contact", handle, sizeof(handle));
            if (handle_n == 0)
                break;
            long long days = extract_int_field(contact_key, "days_since_last");
            long long history = extract_int_field(contact_key, "historical_count");

            if (!any) {
                n = snprintf(out + pos, cap - pos, "Recent social signals:");
                if (n < 0 || (size_t)n >= cap - pos)
                    break;
                pos += (size_t)n;
                any = true;
            }
            n = snprintf(out + pos, cap - pos,
                         "\n- %s has been quiet for %lld days (%lld historical messages)", handle,
                         days, history);
            if (n < 0 || (size_t)n >= cap - pos)
                break;
            pos += (size_t)n;
            rendered++;
            cursor = contact_key + handle_n;
        }
    }

    /* Drift alerts — render PRONOUNCED severity (== 3) only. The
     * snapshot already filters at scan time, so anything in the array
     * is alert-worthy. Render top 2 to avoid prompt bloat. */
    const char *drift_start = strstr(body, "\"drift_alerts\":");
    if (drift_start) {
        size_t rendered = 0;
        const char *cursor = drift_start;
        const char *body_end = body + br;
        while (rendered < 2 && cursor < body_end) {
            const char *contact_key = strstr(cursor, "\"contact\":\"");
            if (!contact_key)
                break;
            char handle[64];
            size_t handle_n = extract_str_field(contact_key, "contact", handle, sizeof(handle));
            if (handle_n == 0)
                break;
            long long dim = extract_int_field(contact_key, "dimension");
            /* dim 0=length, 1=latency, 2=frequency, 3=initiation. */
            const char *dim_name = "behavior";
            if (dim == 0)
                dim_name = "message length";
            else if (dim == 1)
                dim_name = "response latency";
            else if (dim == 2)
                dim_name = "message frequency";
            else if (dim == 3)
                dim_name = "conversation initiation";

            if (!any) {
                n = snprintf(out + pos, cap - pos, "Recent social signals:");
                if (n < 0 || (size_t)n >= cap - pos)
                    break;
                pos += (size_t)n;
                any = true;
            }
            n = snprintf(out + pos, cap - pos, "\n- %s's %s pattern has drifted recently", handle,
                         dim_name);
            if (n < 0 || (size_t)n >= cap - pos)
                break;
            pos += (size_t)n;
            rendered++;
            cursor = contact_key + handle_n;
        }
    }

    if (pos < cap)
        out[pos] = '\0';
    return pos;
}
