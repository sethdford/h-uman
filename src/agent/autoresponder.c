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
                                     char *out, size_t cap) {
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

    char prompt[4096];
    hu_autoresponder_build_prompt(cfg, contact_handle, channel, incoming_text, persona_buf, prompt,
                                  sizeof(prompt));

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
