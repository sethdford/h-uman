#include "seaclaw/channels/imessage.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/process_util.h"
#ifndef SC_CODENAME
#define SC_CODENAME "seaclaw"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#if !SC_IS_TEST && defined(__APPLE__) && defined(__MACH__)
#ifdef SC_ENABLE_SQLITE
#include <sqlite3.h>
#endif
#endif

#define SC_IMESSAGE_SENT_RING_SIZE  8
#define SC_IMESSAGE_SENT_PREFIX_LEN 128

typedef struct sc_imessage_ctx {
    sc_allocator_t *alloc;
    char *default_target;
    size_t default_target_len;
    bool running;
    int64_t last_rowid;
    const char *const *allow_from;
    size_t allow_from_count;
    char sent_ring[SC_IMESSAGE_SENT_RING_SIZE][SC_IMESSAGE_SENT_PREFIX_LEN];
    size_t sent_ring_len[SC_IMESSAGE_SENT_RING_SIZE];
    size_t sent_ring_idx;
} sc_imessage_ctx_t;

#if !SC_IS_TEST && defined(__APPLE__) && defined(__MACH__)
static void imessage_record_sent(sc_imessage_ctx_t *c, const char *msg, size_t msg_len) {
    size_t slot = c->sent_ring_idx % SC_IMESSAGE_SENT_RING_SIZE;
    size_t copy_len =
        msg_len < SC_IMESSAGE_SENT_PREFIX_LEN - 1 ? msg_len : SC_IMESSAGE_SENT_PREFIX_LEN - 1;
    memcpy(c->sent_ring[slot], msg, copy_len);
    c->sent_ring[slot][copy_len] = '\0';
    c->sent_ring_len[slot] = copy_len;
    c->sent_ring_idx++;
}

static bool imessage_was_sent_by_us(sc_imessage_ctx_t *c, const char *text, size_t text_len) {
    for (size_t i = 0; i < SC_IMESSAGE_SENT_RING_SIZE; i++) {
        size_t slen = c->sent_ring_len[i];
        if (slen == 0)
            continue;
        size_t cmp_len = text_len < slen ? text_len : slen;
        if (cmp_len > 0 && memcmp(text, c->sent_ring[i], cmp_len) == 0)
            return true;
    }
    return false;
}
#endif

static sc_error_t imessage_start(void *ctx) {
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void imessage_stop(void *ctx) {
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ctx;
    if (c)
        c->running = false;
}

#if (defined(__APPLE__) && defined(__MACH__)) || SC_IS_TEST
/* Escape " and \ for AppleScript string literal */
size_t escape_for_applescript(char *out, size_t out_cap, const char *in, size_t in_len) {
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 2 < out_cap; i++) {
        if (in[i] == '\\' || in[i] == '"') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
    return j;
}
#endif

static sc_error_t imessage_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
    (void)media;
    (void)media_count;
#if !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    return SC_ERR_NOT_SUPPORTED;
#elif SC_IS_TEST
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    return SC_OK;
#else
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ctx;
    /* Use target if provided, else default_target */
    const char *tgt = target;
    size_t tgt_len = target_len;
    if ((!tgt || tgt_len == 0) && c->default_target && c->default_target_len > 0) {
        tgt = c->default_target;
        tgt_len = c->default_target_len;
    }
    if (!c || !c->alloc || !tgt || tgt_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    /*
     * Post-processing: strip markdown and sanitize AI-sounding phrases
     * before sending via iMessage. Works in-place on a mutable copy.
     */
    size_t clean_cap = message_len + 1;
    char *clean = (char *)c->alloc->alloc(c->alloc->ctx, clean_cap);
    if (!clean)
        return SC_ERR_OUT_OF_MEMORY;
    {
        size_t out_i = 0;
        size_t i = 0;
        while (i < message_len) {
            if (message[i] == '*') {
                while (i < message_len && message[i] == '*')
                    i++;
                continue;
            }
            if ((i == 0 || message[i - 1] == '\n') && message[i] == '#') {
                while (i < message_len && message[i] == '#')
                    i++;
                if (i < message_len && message[i] == ' ')
                    i++;
                continue;
            }
            if ((i == 0 || message[i - 1] == '\n') && i + 1 < message_len &&
                (message[i] == '-' || message[i] == '*') && message[i + 1] == ' ') {
                i += 2;
                continue;
            }
            if (message[i] == '`') {
                i++;
                continue;
            }
            clean[out_i++] = message[i];
            i++;
        }
        clean[out_i] = '\0';
        message = clean;
        message_len = out_i;
    }

    sc_error_t send_err = SC_OK;

    /* Escaped strings: worst case 2x length */
    size_t msg_esc_cap = message_len * 2 + 1;
    size_t tgt_esc_cap = tgt_len * 2 + 1;
    if (msg_esc_cap > 65536 || tgt_esc_cap > 4096) {
        send_err = SC_ERR_INVALID_ARGUMENT;
        goto imsg_cleanup;
    }

    char *msg_esc = (char *)c->alloc->alloc(c->alloc->ctx, msg_esc_cap);
    char *tgt_esc = (char *)c->alloc->alloc(c->alloc->ctx, tgt_esc_cap);
    if (!msg_esc || !tgt_esc) {
        if (msg_esc)
            c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
        if (tgt_esc)
            c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
        send_err = SC_ERR_OUT_OF_MEMORY;
        goto imsg_cleanup;
    }
    escape_for_applescript(msg_esc, msg_esc_cap, message, message_len);
    escape_for_applescript(tgt_esc, tgt_esc_cap, tgt, tgt_len);

    /* Target the iMessage service explicitly for reliability on modern macOS */
    size_t script_cap = 256 + strlen(msg_esc) + strlen(tgt_esc);
    char *script = (char *)c->alloc->alloc(c->alloc->ctx, script_cap);
    if (!script) {
        c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
        c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
        send_err = SC_ERR_OUT_OF_MEMORY;
        goto imsg_cleanup;
    }
    int n = snprintf(script, script_cap,
                     "tell application \"Messages\"\n"
                     "  set targetService to 1st service whose service type = iMessage\n"
                     "  set targetBuddy to buddy \"%s\" of targetService\n"
                     "  send \"%s\" to targetBuddy\n"
                     "end tell",
                     tgt_esc, msg_esc);
    c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
    c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
    if (n < 0 || (size_t)n >= script_cap) {
        c->alloc->free(c->alloc->ctx, script, script_cap);
        send_err = SC_ERR_INTERNAL;
        goto imsg_cleanup;
    }

    /* Human-like typing delay before sending */
    {
        unsigned int delay_ms = (unsigned int)(message_len * 25);
        if (delay_ms < 800)
            delay_ms = 800;
        if (delay_ms > 4000)
            delay_ms = 4000;
        usleep(delay_ms * 1000);
    }

    {
        const char *argv[] = {"osascript", "-e", script, NULL};
        sc_run_result_t result = {0};
        sc_error_t err = sc_process_run(c->alloc, argv, NULL, 65536, &result);
        c->alloc->free(c->alloc->ctx, script, script_cap);
        bool ok = (err == SC_OK && result.success && result.exit_code == 0);
        sc_run_result_free(c->alloc, &result);
        if (err || !ok)
            send_err = SC_ERR_CHANNEL_SEND;
    }

    if (send_err == SC_OK)
        imessage_record_sent(c, message, message_len);

imsg_cleanup:
    c->alloc->free(c->alloc->ctx, clean, clean_cap);
    return send_err;
#endif
}

static const char *imessage_name(void *ctx) {
    (void)ctx;
    return "imessage";
}
static bool imessage_health_check(void *ctx) {
    (void)ctx;
#if !defined(__APPLE__) || !defined(__MACH__)
    return false;
#else
    const char *home = getenv("HOME");
    if (!home)
        return false;
    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return false;
    if (access(db_path, R_OK) != 0) {
        fprintf(
            stderr,
            "[%s] imessage: ~/Library/Messages/chat.db not readable (Full Disk Access required)\n",
            SC_CODENAME);
        return false;
    }
    return true;
#endif
}

static const sc_channel_vtable_t imessage_vtable = {
    .start = imessage_start,
    .stop = imessage_stop,
    .send = imessage_send,
    .name = imessage_name,
    .health_check = imessage_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_imessage_create(sc_allocator_t *alloc, const char *default_target,
                              size_t default_target_len, const char *const *allow_from,
                              size_t allow_from_count, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->default_target = NULL;
    c->default_target_len = 0;
    c->allow_from = allow_from;
    c->allow_from_count = allow_from_count;
    if (default_target && default_target_len > 0) {
        c->default_target = (char *)malloc(default_target_len + 1);
        if (!c->default_target) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->default_target, default_target, default_target_len);
        c->default_target[default_target_len] = '\0';
        c->default_target_len = default_target_len;
    }
    /* Seed last_rowid to current max so we only pick up NEW messages */
#if !SC_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(SC_ENABLE_SQLITE)
    {
        const char *home_env = getenv("HOME");
        if (home_env) {
            char db_path[512];
            int dn = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home_env);
            if (dn > 0 && (size_t)dn < sizeof(db_path)) {
                sqlite3 *db = NULL;
                if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
                    sqlite3_stmt *stmt = NULL;
                    if (sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &stmt, NULL) ==
                        SQLITE_OK) {
                        if (sqlite3_step(stmt) == SQLITE_ROW)
                            c->last_rowid = sqlite3_column_int64(stmt, 0);
                        sqlite3_finalize(stmt);
                    }
                    sqlite3_close(db);
                }
            }
        }
    }
#endif

    out->ctx = c;
    out->vtable = &imessage_vtable;
    return SC_OK;
}

bool sc_imessage_is_configured(sc_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ch->ctx;
    return c->default_target != NULL && c->default_target_len > 0;
}

void sc_imessage_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)ch->ctx;
        sc_allocator_t *a = c->alloc;
        if (c->default_target)
            free(c->default_target);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

/* ── iMessage polling via ~/Library/Messages/chat.db ──────────────────── */

sc_error_t sc_imessage_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    (void)alloc;
    if (!channel_ctx || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if SC_IS_TEST
    (void)max_msgs;
    return SC_OK;
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)max_msgs;
    return SC_ERR_NOT_SUPPORTED;
#elif !defined(SC_ENABLE_SQLITE)
    (void)max_msgs;
    return SC_ERR_NOT_SUPPORTED;
#else
    sc_imessage_ctx_t *c = (sc_imessage_ctx_t *)channel_ctx;

    const char *home = getenv("HOME");
    if (!home)
        return SC_ERR_NOT_SUPPORTED;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return SC_ERR_INTERNAL;

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return SC_ERR_IO;
    }

    const char *sql = "SELECT m.ROWID, m.text, h.id "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE m.is_from_me = 0 AND m.text IS NOT NULL "
                      "AND m.ROWID > ? "
                      "ORDER BY m.ROWID ASC LIMIT ?";

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return SC_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, c->last_rowid);
    sqlite3_bind_int(stmt, 2, (int)max_msgs);

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_msgs) {
        int64_t rowid = sqlite3_column_int64(stmt, 0);
        const char *text = (const char *)sqlite3_column_text(stmt, 1);
        const char *handle = (const char *)sqlite3_column_text(stmt, 2);

        if (!text || !handle)
            continue;

        /* Skip messages that match content we recently sent (echo prevention) */
        if (imessage_was_sent_by_us(c, text, strlen(text))) {
            c->last_rowid = rowid;
            continue;
        }

        size_t handle_len = strlen(handle);
        if (c->allow_from_count > 0) {
            bool allowed = false;
            for (size_t i = 0; i < c->allow_from_count; i++) {
                const char *a = c->allow_from[i];
                if (!a)
                    continue;
                size_t a_len = strlen(a);
                if (a_len == handle_len && strncasecmp(handle, a, a_len) == 0) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed)
                continue;
        }
        size_t text_len = strlen(text);
        if (handle_len >= sizeof(msgs[count].session_key))
            handle_len = sizeof(msgs[count].session_key) - 1;
        if (text_len >= sizeof(msgs[count].content))
            text_len = sizeof(msgs[count].content) - 1;

        memcpy(msgs[count].session_key, handle, handle_len);
        msgs[count].session_key[handle_len] = '\0';
        memcpy(msgs[count].content, text, text_len);
        msgs[count].content[text_len] = '\0';

        c->last_rowid = rowid;
        count++;
        fprintf(stderr, "[imessage] received from %s: %.*s\n", handle,
                (int)(text_len > 80 ? 80 : text_len), text);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    *out_count = count;
    return SC_OK;
#endif
}
