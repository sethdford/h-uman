/* src/agent/autoresponder.c
 *
 * Persona-aware autoresponder. See header for contract + anti-goals.
 * Sprint B Story 3 (docs/plans/2026-05-19-sprint-backlog.md).
 *
 * Layering: pure predicates + prompt builder always compile and are
 * unit-testable. The end-to-end generator path is gated by
 * !HU_IS_TEST when calling the provider (tests stub by calling
 * sanitize directly with synthetic provider output). */

#include "human/autoresponder.h"

#include "human/config.h"
#include "human/core/log.h"
#include "human/memory/personal_model.h"
#include "human/providers/factory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── sanitize fact fields before prompt injection ──────────────────── */

/* Strip prompt-injection-shaped characters from a fact field before
 * inlining into the system prompt. Removes '<', '>', '`', and ASCII
 * control chars (other than space and tab). Truncates at out_cap-1.
 * Returns bytes written (excluding null).
 *
 * Why: Sprint 48 US-48-2 security finding. Fact fields come from
 * inbound contact messages; we don't trust arbitrary content to land
 * in the LLM system prompt unescaped. A contact sending
 * "i like X that <system>DANGEROUS</system>" could inject prompt-shaped
 * content that the LLM might honor. */
static size_t sanitize_fact_field_for_prompt(const char *in, char *out, size_t out_cap) {
    if (!in || !out || out_cap == 0)
        return 0;
    if (out_cap == 1) {
        out[0] = '\0';
        return 0;
    }

    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j < out_cap - 1; i++) {
        unsigned char c = (unsigned char)in[i];

        /* Skip dangerous characters: '<', '>', '`' */
        if (c == '<' || c == '>' || c == '`')
            continue;

        /* Skip ASCII control chars except space (0x20) and tab (0x09) */
        if (c < 0x20 && c != 0x09)
            continue;

        out[j++] = (char)c;
    }
    out[j] = '\0';
    return j;
}

/* ── allowlist (case-insensitive exact match) ───────────────────────── */

bool hu_autoresponder_handle_allowlisted(const hu_autoresponder_config_t *cfg,
                                         const char *contact_handle) {
    if (!cfg || !contact_handle || !*contact_handle)
        return false;
    if (cfg->allowlist_count == 0)
        return false;
    if (cfg->allowlist_count > HU_AUTORESPONDER_MAX_ALLOWLIST)
        return false;
    for (size_t i = 0; i < cfg->allowlist_count; i++) {
        if (cfg->allowlist[i][0] == '\0')
            continue;
        if (strncasecmp(cfg->allowlist[i], contact_handle, HU_AUTORESPONDER_HANDLE_MAX) == 0)
            return true;
    }
    return false;
}

/* ── DND schedule predicate ─────────────────────────────────────────── */

bool hu_autoresponder_in_dnd_window(const hu_autoresponder_config_t *cfg, int64_t now_unix,
                                    int32_t tz_offset_seconds) {
    if (!cfg)
        return false;
    if (cfg->schedule_count == 0)
        return false;
    if (cfg->schedule_count > HU_AUTORESPONDER_MAX_SCHEDULES)
        return false;

    /* Compute local minute-of-day and day-of-week from the unix
     * timestamp + tz offset. Pure arithmetic — no gmtime/localtime. */
    int64_t local_sec = now_unix + (int64_t)tz_offset_seconds;
    /* days since Unix epoch (Thu 1970-01-01 = dow 4). */
    int64_t days_since_epoch = local_sec / 86400;
    if (local_sec < 0 && (local_sec % 86400) != 0)
        days_since_epoch -= 1;
    int dow = (int)(((days_since_epoch % 7) + 4) % 7); /* 0=Sun..6=Sat */
    if (dow < 0)
        dow += 7;

    int64_t day_start = days_since_epoch * 86400;
    int minute_of_day = (int)((local_sec - day_start) / 60);
    if (minute_of_day < 0)
        minute_of_day = 0;
    if (minute_of_day > 1439)
        minute_of_day = 1439;

    for (size_t i = 0; i < cfg->schedule_count; i++) {
        const hu_dnd_schedule_t *s = &cfg->dnd_schedule[i];
        if (!(s->days_of_week_mask & (1u << dow)))
            continue;
        int start = s->start_minute_of_day;
        int end = s->end_minute_of_day;
        if (start == end)
            continue; /* zero-length window */
        if (start < end) {
            /* Normal window: [start, end). */
            if (minute_of_day >= start && minute_of_day < end)
                return true;
        } else {
            /* Wrapped window: [start, 1440) ∪ [0, end). */
            if (minute_of_day >= start || minute_of_day < end)
                return true;
        }
    }
    return false;
}

/* ── top-level should-respond predicate ─────────────────────────────── */

bool hu_autoresponder_should_respond(const hu_autoresponder_config_t *cfg,
                                     const char *contact_handle, int64_t now_unix,
                                     int32_t tz_offset_seconds) {
    if (!cfg || !cfg->enabled)
        return false;
    if (!hu_autoresponder_handle_allowlisted(cfg, contact_handle))
        return false;
    if (!hu_autoresponder_in_dnd_window(cfg, now_unix, tz_offset_seconds))
        return false;
    return true;
}

/* ── safe-string helpers (mirrors src/predictive/drafts.c) ──────────── */

static void sb_append(char *dst, size_t cap, size_t *off, const char *s) {
    if (!dst || cap == 0 || *off + 1 >= cap || !s)
        return;
    size_t avail = cap - *off - 1;
    size_t i = 0;
    while (s[i] && i < avail) {
        dst[*off + i] = s[i];
        i++;
    }
    *off += i;
    dst[*off] = '\0';
}

/* ── prompt builder ─────────────────────────────────────────────────── */

size_t hu_autoresponder_build_prompt(const hu_autoresponder_config_t *cfg,
                                     const char *contact_handle, const char *channel,
                                     const char *incoming_text, const char *persona_summary,
                                     const struct hu_personal_model *contact_model,
                                     int64_t now_unix, char *out, size_t cap) {
    if (!out || cap < 2)
        return 0;
    out[0] = '\0';
    size_t n = 0;

    const char *user_name =
        (cfg && cfg->user_display_name[0]) ? cfg->user_display_name : "the user";

    sb_append(out, cap, &n, "You are auto-replying on behalf of ");
    sb_append(out, cap, &n, user_name);
    sb_append(out, cap, &n, ", who is currently unreachable (DND).\n\n");
    sb_append(out, cap, &n, "STRICT RULES:\n");
    sb_append(out, cap, &n,
              "- Frame yourself as the user's assistant. Start with: \"Hey, this is ");
    sb_append(out, cap, &n, user_name);
    sb_append(out, cap, &n,
              "'s assistant.\" (or equivalent honest framing)\n"
              "- NEVER claim to be the user.\n"
              "- NEVER mention location, calendar, schedule details, or sensitive info.\n"
              "- Keep the reply brief and natural — under 2 sentences.\n"
              "- Match the user's persona voice (see below).\n\n");

    if (contact_handle && *contact_handle) {
        sb_append(out, cap, &n, "Replying to: ");
        sb_append(out, cap, &n, contact_handle);
        sb_append(out, cap, &n, "\n");
    }
    if (channel && *channel) {
        sb_append(out, cap, &n, "Channel: ");
        sb_append(out, cap, &n, channel);
        sb_append(out, cap, &n, "\n");
    }
    if (persona_summary && *persona_summary) {
        sb_append(out, cap, &n, "\nPersona context:\n");
        sb_append(out, cap, &n, persona_summary);
        sb_append(out, cap, &n, "\n");
    }

    /* Sprint 48 US-48-2: Inject contact insights (top-3 facts by effective confidence). */
    if (contact_model && contact_model->fact_count > 0) {
        typedef struct {
            const hu_heuristic_fact_t *fact;
            float eff_conf;
        } scored_fact_t;

        scored_fact_t scored[HU_PM_MAX_FACTS];
        size_t scored_count = 0;
        scored_fact_t tmp;

        /* Collect facts with effective confidence >= 0.1. */
        for (size_t i = 0; i < contact_model->fact_count && scored_count < HU_PM_MAX_FACTS; i++) {
            float eff = hu_heuristic_fact_effective_confidence(&contact_model->facts[i], now_unix);
            if (eff >= 0.1f) {
                scored[scored_count].fact = &contact_model->facts[i];
                scored[scored_count].eff_conf = eff;
                scored_count++;
            }
        }

        /* Bubble-sort top-3 by effective confidence (descending). */
        for (size_t i = 0; i + 1 < scored_count && i < 2; i++) {
            for (size_t j = i + 1; j < scored_count; j++) {
                if (scored[j].eff_conf > scored[i].eff_conf) {
                    tmp = scored[i];
                    scored[i] = scored[j];
                    scored[j] = tmp;
                }
            }
        }

        /* Sprint 48 US-48-2 R4: emit top-3 facts with prompt-injection sanitization.
         * Each field is stripped of <, >, backticks, and ASCII control chars before
         * appending to the system prompt — see sanitize_fact_field_for_prompt above. */
        size_t top_k = (scored_count < 3) ? scored_count : 3;
        if (top_k > 0) {
            sb_append(out, cap, &n, "\nContact insights:\n");
            for (size_t i = 0; i < top_k; i++) {
                const hu_heuristic_fact_t *f = scored[i].fact;
                char safe_subject[HU_FACT_MAX_FIELD];
                char safe_predicate[HU_FACT_MAX_FIELD];
                char safe_object[HU_FACT_MAX_FIELD];

                sanitize_fact_field_for_prompt(f->subject, safe_subject, sizeof(safe_subject));
                sanitize_fact_field_for_prompt(f->predicate, safe_predicate,
                                               sizeof(safe_predicate));
                sanitize_fact_field_for_prompt(f->object, safe_object, sizeof(safe_object));

                sb_append(out, cap, &n, "- ");
                sb_append(out, cap, &n, safe_subject);
                sb_append(out, cap, &n, " ");
                sb_append(out, cap, &n, safe_predicate);
                sb_append(out, cap, &n, " ");
                sb_append(out, cap, &n, safe_object);
                sb_append(out, cap, &n, "\n");
            }
        }
    }

    sb_append(out, cap, &n, "\nIncoming message:\n");
    sb_append(out, cap, &n, (incoming_text && *incoming_text) ? incoming_text : "(empty)");
    sb_append(out, cap, &n, "\n\nReply:");
    return n;
}

/* ── sanitize: catch dangerous phrasing post-hoc ────────────────────── */

/* Returns true if `text` contains "I am <user_name>" or
 * "I'm <user_name>" or "it's <user_name>" WITHOUT being followed by
 * "'s assistant" — i.e. the model claimed to be the user. Word-
 * boundary aware. Pure helper. */
static bool reply_falsely_claims_to_be_user(const char *text, const char *user_name) {
    if (!text || !user_name || !*user_name)
        return false;
    static const char *prefixes[] = {"i am ", "i'm ", "it's ", "this is ", NULL};
    char lower_user[HU_AUTORESPONDER_DEFAULT_USER_NAME_MAX];
    size_t user_len = strlen(user_name);
    if (user_len >= sizeof(lower_user))
        user_len = sizeof(lower_user) - 1;
    for (size_t i = 0; i < user_len; i++)
        lower_user[i] = (char)tolower((unsigned char)user_name[i]);
    lower_user[user_len] = '\0';

    /* Scan text lowercased windowed against each prefix. */
    size_t tlen = strlen(text);
    for (size_t i = 0; prefixes[i]; i++) {
        const char *pfx = prefixes[i];
        size_t plen = strlen(pfx);
        if (tlen < plen + user_len)
            continue;
        for (size_t off = 0; off + plen + user_len <= tlen; off++) {
            /* lowercase-compare prefix */
            bool ok = true;
            for (size_t j = 0; j < plen; j++) {
                if (tolower((unsigned char)text[off + j]) != pfx[j]) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;
            /* lowercase-compare user name */
            for (size_t j = 0; j < user_len; j++) {
                if (tolower((unsigned char)text[off + plen + j]) != lower_user[j]) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;
            /* Word-boundary check: char immediately after the name
             * must NOT be alphanumeric (otherwise we matched a
             * larger word). */
            size_t after_idx = off + plen + user_len;
            if (after_idx < tlen && isalnum((unsigned char)text[after_idx]))
                continue;
            /* Look for "'s assistant" within the next ~24 chars; if
             * present, the framing is safe even with the "I'm <name>"
             * prefix. */
            const char *tail = text + after_idx;
            const char *needle = "'s assistant";
            size_t needle_len = strlen(needle);
            if (after_idx + needle_len <= tlen) {
                bool tail_match = true;
                for (size_t j = 0; j < needle_len; j++) {
                    if (tolower((unsigned char)tail[j]) != needle[j]) {
                        tail_match = false;
                        break;
                    }
                }
                if (tail_match)
                    continue;
            }
            return true; /* claimed to be the user */
        }
    }
    return false;
}

size_t hu_autoresponder_sanitize_reply(const hu_autoresponder_config_t *cfg, const char *raw_text,
                                       char *out, size_t cap) {
    if (!out || cap < 2)
        return 0;
    out[0] = '\0';
    const char *user_name =
        (cfg && cfg->user_display_name[0]) ? cfg->user_display_name : "the user";

    if (!raw_text || !*raw_text) {
        int n = snprintf(out, cap, "Hey, this is %s's assistant — they'll get back to you soon.",
                         user_name);
        return n > 0 ? (size_t)((size_t)n < cap ? n : cap - 1) : 0;
    }

    if (reply_falsely_claims_to_be_user(raw_text, user_name)) {
        int n = snprintf(out, cap,
                         "Hey, this is %s's assistant — they're not reachable right now but will "
                         "get back to you when they can.",
                         user_name);
        return n > 0 ? (size_t)((size_t)n < cap ? n : cap - 1) : 0;
    }

    /* Pass through but bounded: cap at HU_AUTORESPONDER_REPLY_MAX. */
    size_t n = strlen(raw_text);
    if (n > HU_AUTORESPONDER_REPLY_MAX)
        n = HU_AUTORESPONDER_REPLY_MAX;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, raw_text, n);
    out[n] = '\0';
    return n;
}

/* ── log-line writer (JSON, one per line) ───────────────────────────── */

static const char *resolve_log_path(const hu_autoresponder_config_t *cfg, char *buf, size_t cap) {
    if (cfg && cfg->log_path[0])
        return cfg->log_path;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    int n = snprintf(buf, cap, "%s/.human/autoresponder.log", home);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}

static void json_escape_into(const char *src, char *dst, size_t cap, size_t *off) {
    if (!dst || cap == 0 || !src)
        return;
    while (*src && *off + 2 < cap) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') {
            if (*off + 3 >= cap)
                break;
            dst[(*off)++] = '\\';
            dst[(*off)++] = (char)c;
        } else if (c < 0x20) {
            /* control char — emit \uXXXX */
            if (*off + 7 >= cap)
                break;
            int n = snprintf(dst + *off, cap - *off, "\\u%04x", c);
            if (n > 0)
                *off += (size_t)n;
        } else {
            dst[(*off)++] = (char)c;
        }
    }
    dst[*off] = '\0';
}

hu_error_t hu_autoresponder_log_reply(const hu_autoresponder_config_t *cfg,
                                      const char *contact_handle, const char *channel,
                                      const char *reply_text, int64_t now_unix) {
    if (!cfg || !contact_handle || !*contact_handle)
        return HU_ERR_INVALID_ARGUMENT;

    char path_buf[512];
    const char *path = resolve_log_path(cfg, path_buf, sizeof(path_buf));
    if (!path)
        return HU_ERR_IO;

    char line[HU_AUTORESPONDER_LOG_LINE_MAX];
    size_t off = 0;
    int n = snprintf(line, sizeof(line), "{\"ts\":%lld,\"contact\":\"", (long long)now_unix);
    if (n < 0 || (size_t)n >= sizeof(line))
        return HU_ERR_IO;
    off = (size_t)n;
    json_escape_into(contact_handle, line, sizeof(line), &off);
    if (off + 16 >= sizeof(line))
        return HU_ERR_IO;
    line[off++] = '"';
    line[off++] = ',';
    /* channel */
    const char *seg = "\"channel\":\"";
    size_t seg_len = strlen(seg);
    if (off + seg_len + 1 >= sizeof(line))
        return HU_ERR_IO;
    memcpy(line + off, seg, seg_len);
    off += seg_len;
    json_escape_into(channel ? channel : "", line, sizeof(line), &off);
    if (off + 16 >= sizeof(line))
        return HU_ERR_IO;
    line[off++] = '"';
    line[off++] = ',';
    /* reply */
    seg = "\"reply\":\"";
    seg_len = strlen(seg);
    if (off + seg_len + 1 >= sizeof(line))
        return HU_ERR_IO;
    memcpy(line + off, seg, seg_len);
    off += seg_len;
    json_escape_into(reply_text ? reply_text : "", line, sizeof(line), &off);
    if (off + 3 >= sizeof(line))
        return HU_ERR_IO;
    line[off++] = '"';
    line[off++] = '}';
    line[off++] = '\n';
    line[off] = '\0';

    /* Append to the log path. We don't fsync — autoresponder logs are
     * informational, and the daemon may write many per minute under
     * heavy load. */
    FILE *fp = fopen(path, "a");
    if (!fp)
        return HU_ERR_IO;
    size_t wrote = fwrite(line, 1, off, fp);
    fclose(fp);
    return (wrote == off) ? HU_OK : HU_ERR_IO;
}

/* ── end-to-end generator ───────────────────────────────────────────── */

hu_error_t hu_autoresponder_generate_reply(hu_allocator_t *alloc,
                                           const hu_autoresponder_config_t *cfg,
                                           const struct hu_personal_model *model,
                                           const char *contact_handle, const char *channel,
                                           const char *incoming_text, int64_t now_unix,
                                           int32_t tz_offset_seconds, char *out, size_t cap) {
    if (!alloc || !cfg || !contact_handle || !*contact_handle || !out || cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    out[0] = '\0';

    if (!hu_autoresponder_should_respond(cfg, contact_handle, now_unix, tz_offset_seconds))
        return HU_ERR_PERMISSION_DENIED;

    /* Build persona summary (best-effort; may be empty). */
    char persona_buf[2048];
    persona_buf[0] = '\0';
    if (model)
        hu_personal_model_build_prompt((const hu_personal_model_t *)model, persona_buf,
                                       sizeof(persona_buf));

    /* Load per-contact personal model (best-effort). */
    hu_personal_model_t contact_model = {0};
    char pm_path[512];
    if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
        (void)hu_personal_model_load_for_contact(&contact_model, contact_handle, pm_path);
    }

    /* Ingest the incoming message into contact model (best-effort). */
    if (incoming_text && *incoming_text) {
        (void)hu_personal_model_ingest_for_contact(&contact_model, contact_handle, incoming_text,
                                                   strlen(incoming_text), /*from_user=*/true,
                                                   now_unix, pm_path);
    }

    char prompt[4096];
    hu_autoresponder_build_prompt(cfg, contact_handle, channel, incoming_text, persona_buf,
                                  &contact_model, now_unix, prompt, sizeof(prompt));

    /* Load default provider. Same pattern as predictive_drafts. */
    hu_config_t loaded_cfg;
    memset(&loaded_cfg, 0, sizeof(loaded_cfg));
    hu_error_t cerr = hu_config_load(alloc, &loaded_cfg);
    if (cerr != HU_OK)
        return HU_ERR_NOT_SUPPORTED;

    hu_provider_t provider;
    memset(&provider, 0, sizeof(provider));
    hu_error_t perr = hu_provider_create_default(alloc, &loaded_cfg, &provider);
    if (perr != HU_OK || !provider.vtable) {
        hu_config_deinit(&loaded_cfg);
        return HU_ERR_NOT_SUPPORTED;
    }
    if (!provider.vtable->chat_with_system) {
        if (provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        hu_config_deinit(&loaded_cfg);
        return HU_ERR_NOT_SUPPORTED;
    }

    static const char *kSystem =
        "You are a careful autoresponder. Follow every rule strictly. Output ONLY the reply text.";
    char *resp = NULL;
    size_t resp_len = 0;
    const char *model_id = loaded_cfg.default_model;
    size_t model_id_len = model_id ? strlen(model_id) : 0;
    hu_error_t cerr2 = provider.vtable->chat_with_system(
        provider.ctx, alloc, kSystem, strlen(kSystem), prompt, strlen(prompt), model_id,
        model_id_len, 0.6, &resp, &resp_len);

    hu_error_t result = HU_OK;
    if (cerr2 != HU_OK || !resp || resp_len == 0) {
        result = HU_ERR_IO;
    } else {
        hu_autoresponder_sanitize_reply(cfg, resp, out, cap);
        /* Log the sanitized reply (best-effort; log failure does NOT
         * fail the call — the user got a reply, the digest is just
         * delayed). */
        (void)hu_autoresponder_log_reply(cfg, contact_handle, channel, out, now_unix);
    }

    if (resp)
        alloc->free(alloc->ctx, resp, resp_len + 1);
    if (provider.vtable->deinit)
        provider.vtable->deinit(provider.ctx, alloc);
    hu_config_deinit(&loaded_cfg);
    return result;
}

/* ── tiny JSON probe (substring-based, no parser dep) ────────────────
 *
 * Mirrors src/persona/social_insights.c::find_key — these helpers are
 * inlined rather than shared because each TU has slightly different
 * shape expectations and the helpers are tiny. Replace with a real
 * parser when 3rd consumer appears. */

static const char *ar_find_key(const char *body, const char *key) {
    if (!body || !key)
        return NULL;
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return NULL;
    const char *p = strstr(body, needle);
    if (!p)
        return NULL;
    p += (size_t)n;
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static size_t ar_extract_str_field(const char *obj, const char *key, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    const char *p = ar_find_key(obj, key);
    if (!p || *p != '"')
        return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            /* simple escape: copy the next char as-is (handles \" \\ \/ \n) */
            char c = p[1];
            if (c == 'n')
                out[i++] = '\n';
            else if (c == 't')
                out[i++] = '\t';
            else
                out[i++] = c;
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return i;
}

static bool ar_extract_bool_field(const char *obj, const char *key, bool default_val) {
    const char *p = ar_find_key(obj, key);
    if (!p)
        return default_val;
    if (strncmp(p, "true", 4) == 0)
        return true;
    if (strncmp(p, "false", 5) == 0)
        return false;
    return default_val;
}

/* Parse "HH:MM" → minute-of-day. Returns -1 on parse failure. */
static int parse_hhmm(const char *s) {
    if (!s)
        return -1;
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2)
        return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59)
        return -1;
    return h * 60 + m;
}

/* Parse "daily" / "weekdays" / "weekends" / "mon,wed,fri" / "0,1,5"
 * → days_of_week_mask. Returns 0 (no days) on failure. */
static uint8_t parse_days_mask(const char *s) {
    if (!s || !*s)
        return 0;
    if (strcasecmp(s, "daily") == 0)
        return HU_DOW_MASK_DAILY;
    if (strcasecmp(s, "weekdays") == 0)
        return HU_DOW_MASK_WEEKDAYS;
    if (strcasecmp(s, "weekends") == 0)
        return HU_DOW_MASK_WEEKENDS;
    uint8_t mask = 0;
    /* Token-by-token, comma-separated. */
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ')
            tok++;
        if (!*tok)
            continue;
        int dow = -1;
        if (strncasecmp(tok, "sun", 3) == 0)
            dow = 0;
        else if (strncasecmp(tok, "mon", 3) == 0)
            dow = 1;
        else if (strncasecmp(tok, "tue", 3) == 0)
            dow = 2;
        else if (strncasecmp(tok, "wed", 3) == 0)
            dow = 3;
        else if (strncasecmp(tok, "thu", 3) == 0)
            dow = 4;
        else if (strncasecmp(tok, "fri", 3) == 0)
            dow = 5;
        else if (strncasecmp(tok, "sat", 3) == 0)
            dow = 6;
        else if (isdigit((unsigned char)*tok))
            dow = atoi(tok);
        if (dow >= 0 && dow <= 6)
            mask |= (uint8_t)(1u << dow);
    }
    return mask;
}

/* Walk the "allowlist": [...] array and fill cfg->allowlist. Lenient:
 * tolerates whitespace and trailing commas; stops at the matching ']'. */
static void parse_allowlist_array(const char *array_start, hu_autoresponder_config_t *cfg) {
    if (!array_start || *array_start != '[')
        return;
    const char *p = array_start + 1;
    while (*p && *p != ']' && cfg->allowlist_count < HU_AUTORESPONDER_MAX_ALLOWLIST) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
            p++;
        if (*p == ']')
            break;
        if (*p != '"')
            break;
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < HU_AUTORESPONDER_HANDLE_MAX) {
            cfg->allowlist[cfg->allowlist_count][i++] = *p++;
        }
        cfg->allowlist[cfg->allowlist_count][i] = '\0';
        cfg->allowlist_count++;
        if (*p == '"')
            p++;
    }
}

/* Walk the "schedules": [{...},{...}] array. Each object should carry
 * "start", "end", and "days" keys. */
static void parse_schedules_array(const char *array_start, hu_autoresponder_config_t *cfg) {
    if (!array_start || *array_start != '[')
        return;
    const char *p = array_start + 1;
    while (*p && *p != ']' && cfg->schedule_count < HU_AUTORESPONDER_MAX_SCHEDULES) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
            p++;
        if (*p == ']')
            break;
        if (*p != '{')
            break;
        /* Find the matching '}' — naive depth-1 scan (we don't expect
         * nested objects inside a schedule entry). */
        const char *obj_start = p;
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '{')
                depth++;
            else if (*p == '}')
                depth--;
            p++;
        }
        size_t obj_len = (size_t)(p - obj_start);
        char obj_buf[256];
        if (obj_len >= sizeof(obj_buf))
            continue;
        memcpy(obj_buf, obj_start, obj_len);
        obj_buf[obj_len] = '\0';

        char start_str[16] = {0};
        char end_str[16] = {0};
        char days_str[64] = {0};
        ar_extract_str_field(obj_buf, "start", start_str, sizeof(start_str));
        ar_extract_str_field(obj_buf, "end", end_str, sizeof(end_str));
        ar_extract_str_field(obj_buf, "days", days_str, sizeof(days_str));

        int start_mod = parse_hhmm(start_str);
        int end_mod = parse_hhmm(end_str);
        uint8_t days = parse_days_mask(days_str[0] ? days_str : "daily");
        if (start_mod < 0 || end_mod < 0 || days == 0)
            continue;

        cfg->dnd_schedule[cfg->schedule_count].start_minute_of_day = (int16_t)start_mod;
        cfg->dnd_schedule[cfg->schedule_count].end_minute_of_day = (int16_t)end_mod;
        cfg->dnd_schedule[cfg->schedule_count].days_of_week_mask = days;
        cfg->schedule_count++;
    }
}

hu_error_t hu_autoresponder_config_load_from_file(const char *path,
                                                  hu_autoresponder_config_t *out) {
    if (!path || !*path || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_NOT_FOUND;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    long fsz = ftell(fp);
    if (fsz < 2 || fsz > 1024 * 1024) {
        fclose(fp);
        return HU_ERR_PARSE;
    }
    rewind(fp);
    char *body = (char *)malloc((size_t)fsz + 1);
    if (!body) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(body, 1, (size_t)fsz, fp);
    fclose(fp);
    if (rd != (size_t)fsz) {
        free(body);
        return HU_ERR_IO;
    }
    body[fsz] = '\0';

    /* Cheap shape check: must contain at least one '{' AND one '}'. */
    if (!strchr(body, '{') || !strchr(body, '}')) {
        free(body);
        return HU_ERR_PARSE;
    }

    out->enabled = ar_extract_bool_field(body, "enabled", false);
    ar_extract_str_field(body, "user_display_name", out->user_display_name,
                         sizeof(out->user_display_name));
    ar_extract_str_field(body, "log_path", out->log_path, sizeof(out->log_path));

    const char *allow = ar_find_key(body, "allowlist");
    if (allow)
        parse_allowlist_array(allow, out);

    const char *scheds = ar_find_key(body, "schedules");
    if (scheds)
        parse_schedules_array(scheds, out);

    free(body);
    return HU_OK;
}

/* ── digest aggregation (A4) ─────────────────────────────────────────── */

/* Increment or insert a per-contact counter. Bounded by allowlist cap
 * to keep the digest output readable. */
static void digest_bump_contact(hu_autoresponder_digest_t *d, const char *handle) {
    if (!d || !handle || !*handle)
        return;
    for (size_t i = 0; i < d->per_contact_count; i++) {
        if (strncasecmp(d->per_contact[i].handle, handle, HU_AUTORESPONDER_HANDLE_MAX) == 0) {
            d->per_contact[i].count++;
            return;
        }
    }
    if (d->per_contact_count >= HU_AUTORESPONDER_MAX_ALLOWLIST)
        return;
    snprintf(d->per_contact[d->per_contact_count].handle, HU_AUTORESPONDER_HANDLE_MAX, "%s",
             handle);
    d->per_contact[d->per_contact_count].count = 1;
    d->per_contact_count++;
}

/* Extract an int64 numeric value from `{"ts":<num>,...}` after a key
 * lookup. Returns -1 on parse failure (timestamp won't pass window
 * gate). */
static int64_t parse_int64_field(const char *obj, const char *key) {
    const char *p = ar_find_key(obj, key);
    if (!p)
        return -1;
    /* Skip whitespace; expect digits. */
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || (!isdigit((unsigned char)*p) && *p != '-'))
        return -1;
    char *endp = NULL;
    long long v = strtoll(p, &endp, 10);
    if (endp == p)
        return -1;
    return (int64_t)v;
}

void hu_autoresponder_digest_aggregate(const char *body, int64_t now_unix, int64_t since_seconds,
                                       hu_autoresponder_digest_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!body || !*body)
        return;
    if (since_seconds <= 0)
        since_seconds = 86400; /* 24h default for safety */
    int64_t window_start = now_unix - since_seconds;

    /* Walk line-by-line. Each line should be a JSON object with "ts"
     * and "contact" fields. Robust to malformed lines. */
    const char *line_start = body;
    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        if (line_len < 8) {
            /* Too short to be meaningful — skip. */
            line_start = line_end ? line_end + 1 : line_start + line_len;
            continue;
        }
        /* Copy the line into a bounded buffer so the helpers can
         * NUL-terminate. */
        char line_buf[HU_AUTORESPONDER_LOG_LINE_MAX];
        size_t copy = line_len >= sizeof(line_buf) ? sizeof(line_buf) - 1 : line_len;
        memcpy(line_buf, line_start, copy);
        line_buf[copy] = '\0';

        int64_t ts = parse_int64_field(line_buf, "ts");
        if (ts >= window_start && ts <= now_unix + 60 /* tolerate small clock skew */) {
            char handle[HU_AUTORESPONDER_HANDLE_MAX] = {0};
            if (ar_extract_str_field(line_buf, "contact", handle, sizeof(handle)) > 0) {
                out->total_replies++;
                digest_bump_contact(out, handle);
            }
        }
        if (!line_end)
            break;
        line_start = line_end + 1;
    }
}

/* ── CLI: human autoresponder digest ─────────────────────────────────── */

static const char *kAutoresponderUsage =
    "Usage: human autoresponder digest [--since-hours N] [--log <path>]\n"
    "  Aggregates ~/.human/autoresponder.log (or --log) into a per-contact\n"
    "  reply count for the last N hours (default 24).\n";

hu_error_t cmd_autoresponder(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    if (argc < 3 || strcmp(argv[2], "digest") != 0) {
        printf("%s", kAutoresponderUsage);
        return HU_ERR_INVALID_ARGUMENT;
    }
    int since_hours = 24;
    const char *log_path_override = NULL;
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--since-hours") == 0 && i + 1 < argc) {
            since_hours = atoi(argv[++i]);
            if (since_hours <= 0)
                since_hours = 24;
        } else if (strcmp(a, "--log") == 0 && i + 1 < argc) {
            log_path_override = argv[++i];
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printf("%s", kAutoresponderUsage);
            return HU_OK;
        }
    }

    char path_buf[512];
    const char *path = log_path_override;
    if (!path) {
        const char *home = getenv("HOME");
        if (home && home[0] &&
            snprintf(path_buf, sizeof(path_buf), "%s/.human/autoresponder.log", home) > 0) {
            path = path_buf;
        }
    }
    if (!path) {
        fprintf(stderr, "could not resolve log path\n");
        return HU_ERR_IO;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        /* No file → 0 replies; still a successful digest. */
        printf("0 autoresponder replies in the last %d hours (no log at %s)\n", since_hours, path);
        return HU_OK;
    }
    /* Bounded read — autoresponder.log shouldn't grow unbounded but
     * cap at 4 MB to keep digest cheap. */
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    long fsz = ftell(fp);
    if (fsz < 0)
        fsz = 0;
    if (fsz > 4 * 1024 * 1024)
        fsz = 4 * 1024 * 1024; /* cap */
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    char *body = (char *)malloc((size_t)fsz + 1);
    if (!body) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(body, 1, (size_t)fsz, fp);
    fclose(fp);
    body[rd] = '\0';

    hu_autoresponder_digest_t d;
    hu_autoresponder_digest_aggregate(body, (int64_t)time(NULL), (int64_t)since_hours * 3600, &d);
    free(body);

    printf("%d autoresponder replies in the last %d hours:\n", (int)d.total_replies, since_hours);
    for (size_t i = 0; i < d.per_contact_count; i++) {
        printf("  %4d  %s\n", (int)d.per_contact[i].count, d.per_contact[i].handle);
    }
    if (d.per_contact_count == 0)
        printf("  (no replies in window)\n");
    return HU_OK;
}
