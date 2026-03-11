#include "human/channels/imessage.h"
#include "human/channel_loop.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#ifndef HU_CODENAME
#define HU_CODENAME "human"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif
#endif

#define HU_IMESSAGE_SENT_RING_SIZE  32
#define HU_IMESSAGE_SENT_PREFIX_LEN 256

typedef struct hu_imessage_ctx {
    hu_allocator_t *alloc;
    char *default_target;
    size_t default_target_len;
    bool running;
    int64_t last_rowid;
    const char *const *allow_from;
    size_t allow_from_count;
    char sent_ring[HU_IMESSAGE_SENT_RING_SIZE][HU_IMESSAGE_SENT_PREFIX_LEN];
    size_t sent_ring_len[HU_IMESSAGE_SENT_RING_SIZE];
    uint32_t sent_ring_hash[HU_IMESSAGE_SENT_RING_SIZE];
    size_t sent_ring_idx;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
        bool has_attachment;
        bool has_video;
    } mock_msgs[8];
    size_t mock_count;
    hu_reaction_type_t last_reaction;
    int64_t last_reaction_message_id;
#endif
} hu_imessage_ctx_t;

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
static uint32_t imessage_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static void imessage_record_sent(hu_imessage_ctx_t *c, const char *msg, size_t msg_len) {
    size_t slot = c->sent_ring_idx % HU_IMESSAGE_SENT_RING_SIZE;
    size_t copy_len =
        msg_len < HU_IMESSAGE_SENT_PREFIX_LEN - 1 ? msg_len : HU_IMESSAGE_SENT_PREFIX_LEN - 1;
    memcpy(c->sent_ring[slot], msg, copy_len);
    c->sent_ring[slot][copy_len] = '\0';
    c->sent_ring_len[slot] = copy_len;
    c->sent_ring_hash[slot] = imessage_hash(msg, msg_len);
    c->sent_ring_idx++;
}

static bool imessage_was_sent_by_us(hu_imessage_ctx_t *c, const char *text, size_t text_len) {
    uint32_t h = imessage_hash(text, text_len);
    for (size_t i = 0; i < HU_IMESSAGE_SENT_RING_SIZE; i++) {
        size_t slen = c->sent_ring_len[i];
        if (slen == 0)
            continue;
        if (c->sent_ring_hash[i] == h) {
            size_t cmp_len = text_len < slen ? text_len : slen;
            if (cmp_len > 0 && memcmp(text, c->sent_ring[i], cmp_len) == 0)
                return true;
        }
    }
    return false;
}

#ifdef HU_ENABLE_SQLITE
bool hu_imessage_user_responded_recently(void *channel_ctx, const char *handle, size_t handle_len,
                                         int within_seconds) {
    (void)channel_ctx;
    if (!handle || handle_len == 0 || within_seconds <= 0)
        return false;

    const char *home = getenv("HOME");
    if (!home)
        return false;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return false;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return false;
    }

    /*
     * Check for is_from_me=1 messages to this handle within the time window.
     * macOS Messages stores dates as nanoseconds since 2001-01-01 (Core Data epoch).
     */
    const char *sql = "SELECT COUNT(*) FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 1 "
                      "AND m.date > ((?2 - 978307200) * 1000000000)";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, handle, (int)handle_len, NULL);

    time_t cutoff = time(NULL) - within_seconds;
    sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = sqlite3_column_int(stmt, 0) > 0;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
}
#endif
#endif

static hu_error_t imessage_start(void *ctx) {
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void imessage_stop(void *ctx) {
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (c)
        c->running = false;
}

#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST

#if !HU_IS_TEST
/* Safety net: remove AI-sounding phrases that slipped through. Primary mechanism is
 * prompt-level guidance in hu_conversation_build_awareness. Modifies in-place.
 * Returns new length. Case-insensitive except "Absolutely! " (capital A only). */
static size_t imessage_sanitize_output(char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;

    static const struct {
        const char *phrase;
        size_t phrase_len;
        bool case_sensitive;
    } phrases[] = {
        {"I'd be happy to ", 16, false},
        {"Great question! ", 16, false},
        {"That's a great question", 23, false},
        {"Let me know if you need anything", 32, false},
        {"Let me know if ", 15, false},
        {"Feel free to ", 13, false},
        {"Absolutely! ", 12, true},
        {"I understand your ", 18, false},
        {"I appreciate ", 13, false},
        {"Here's what I think: ", 21, false},
        {"I hope this helps", 17, false},
        {"Don't hesitate to ", 18, false},
        {"I'm here to help", 16, false},
        {"As an AI", 8, false},
        {"As a language model", 19, false},
    };

    for (;;) {
        bool changed = false;
        for (size_t p = 0; p < sizeof(phrases) / sizeof(phrases[0]); p++) {
            const char *needle = phrases[p].phrase;
            size_t needle_len = phrases[p].phrase_len;
            if (needle_len > len)
                continue;

            char *pos = buf;
            while (pos + needle_len <= buf + len) {
                bool match = false;
                if (phrases[p].case_sensitive) {
                    match = (memcmp(pos, needle, needle_len) == 0);
                } else {
                    match = (strncasecmp(pos, needle, needle_len) == 0);
                }
                if (match) {
                    memmove(pos, pos + needle_len, (size_t)((buf + len) - (pos + needle_len)) + 1);
                    len -= needle_len;
                    buf[len] = '\0';
                    changed = true;
                    break;
                }
                pos++;
            }
            if (changed)
                break;
        }
        if (!changed)
            break;
    }

    /* Collapse double spaces */
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == ' ' && buf[i - 1] == ' ') {
            memmove(buf + i, buf + i + 1, len - i);
            len--;
            i--;
        }
    }

    /* Trim leading whitespace */
    while (len > 0 && (buf[0] == ' ' || buf[0] == '\t')) {
        memmove(buf, buf + 1, len);
        len--;
    }

    /* Trim trailing whitespace */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
        buf[--len] = '\0';
    }

    return len;
}
#endif /* !HU_IS_TEST */

const char *hu_imessage_reaction_to_tapback_name(hu_reaction_type_t reaction) {
    switch (reaction) {
    case HU_REACTION_HEART:
        return "love";
    case HU_REACTION_THUMBS_UP:
        return "like";
    case HU_REACTION_THUMBS_DOWN:
        return "dislike";
    case HU_REACTION_HAHA:
        return "laugh";
    case HU_REACTION_EMPHASIS:
        return "emphasize";
    case HU_REACTION_QUESTION:
        return "question";
    default:
        return NULL;
    }
}

#if defined(HU_IMESSAGE_TAPBACK_ENABLED)
/* Maps reaction type to iMessage AX context menu label (Love, Like, Ha Ha, etc.). */
static const char *imessage_reaction_to_ax_menu_name(hu_reaction_type_t reaction) {
    switch (reaction) {
    case HU_REACTION_HEART:
        return "Love";
    case HU_REACTION_THUMBS_UP:
        return "Like";
    case HU_REACTION_THUMBS_DOWN:
        return "Dislike";
    case HU_REACTION_HAHA:
        return "Ha Ha";
    case HU_REACTION_EMPHASIS:
        return "!!";
    case HU_REACTION_QUESTION:
        return "?";
    default:
        return NULL;
    }
}
#endif

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

static hu_error_t imessage_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
#if HU_IS_TEST
    (void)media;
    (void)media_count;
    {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        c->last_message[len] = '\0';
        c->last_message_len = len;
        return HU_OK;
    }
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    /* Use target if provided, else default_target */
    const char *tgt = target;
    size_t tgt_len = target_len;
    if ((!tgt || tgt_len == 0) && c->default_target && c->default_target_len > 0) {
        tgt = c->default_target;
        tgt_len = c->default_target_len;
    }
    if (!c || !c->alloc || !tgt || tgt_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (message_len == 0 && media_count == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (message_len > 0 && !message)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t send_err = HU_OK;
    char *clean = NULL;
    size_t clean_cap = 0;

    /* Skip empty text send when we have media (voice-only) */
    if (message_len > 0) {
        /*
         * Post-processing: strip markdown and sanitize AI-sounding phrases
         * before sending via iMessage. Works in-place on a mutable copy.
         */
        clean_cap = message_len + 1;
        clean = (char *)c->alloc->alloc(c->alloc->ctx, clean_cap);
        if (!clean)
            return HU_ERR_OUT_OF_MEMORY;
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

        message_len = imessage_sanitize_output(clean, message_len);

        /* Hard length cap for iMessage: truncate at sentence boundary near 300 chars */
        if (message_len > 300) {
            size_t cut = 300;
            while (cut > 100 && message[cut] != '.' && message[cut] != '!' && message[cut] != '?')
                cut--;
            if (cut > 100) {
                message_len = cut + 1;
                clean[message_len] = '\0';
            } else {
                size_t space_cut = 300;
                while (space_cut > 100 && message[space_cut] != ' ')
                    space_cut--;
                if (space_cut > 100)
                    message_len = space_cut;
                else
                    message_len = 300;
                clean[message_len] = '\0';
            }
        }

        /* Escaped strings: worst case 2x length */
        size_t msg_esc_cap = message_len * 2 + 1;
        size_t tgt_esc_cap = tgt_len * 2 + 1;
        if (msg_esc_cap > 65536 || tgt_esc_cap > 4096) {
            send_err = HU_ERR_INVALID_ARGUMENT;
            goto imsg_cleanup;
        }

        char *msg_esc = (char *)c->alloc->alloc(c->alloc->ctx, msg_esc_cap);
        char *tgt_esc = (char *)c->alloc->alloc(c->alloc->ctx, tgt_esc_cap);
        if (!msg_esc || !tgt_esc) {
            if (msg_esc)
                c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
            if (tgt_esc)
                c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
            send_err = HU_ERR_OUT_OF_MEMORY;
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
            send_err = HU_ERR_OUT_OF_MEMORY;
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
            send_err = HU_ERR_INTERNAL;
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
            hu_run_result_t result = {0};
            hu_error_t err = hu_process_run(c->alloc, argv, NULL, 65536, &result);
            c->alloc->free(c->alloc->ctx, script, script_cap);
            bool ok = (err == HU_OK && result.success && result.exit_code == 0);
            hu_run_result_free(c->alloc, &result);
            if (err || !ok)
                send_err = HU_ERR_CHANNEL_SEND;
        }

        if (send_err == HU_OK)
            imessage_record_sent(c, message, message_len);
    }

#if !HU_IS_TEST
    /* Send media attachments (local file paths only) after text succeeds */
    if (send_err == HU_OK && media && media_count > 0) {
        size_t m_tgt_cap = tgt_len * 2 + 1;
        if (m_tgt_cap <= 4096) {
            char *m_tgt_esc = (char *)c->alloc->alloc(c->alloc->ctx, m_tgt_cap);
            if (m_tgt_esc) {
                escape_for_applescript(m_tgt_esc, m_tgt_cap, tgt, tgt_len);
                for (size_t i = 0; i < media_count && send_err == HU_OK; i++) {
                    const char *url = media[i];
                    if (!url || url[0] != '/')
                        continue; /* Skip URLs and non-file media */
                    size_t path_len = strlen(url);
                    size_t path_esc_cap = path_len * 2 + 1;
                    if (path_esc_cap > 8192)
                        continue;
                    char *path_esc = (char *)c->alloc->alloc(c->alloc->ctx, path_esc_cap);
                    if (!path_esc)
                        continue;
                    escape_for_applescript(path_esc, path_esc_cap, url, path_len);
                    size_t m_script_cap = 256 + strlen(m_tgt_esc) + strlen(path_esc);
                    char *m_script = (char *)c->alloc->alloc(c->alloc->ctx, m_script_cap);
                    if (m_script) {
                        int m_n = snprintf(
                            m_script, m_script_cap,
                            "tell application \"Messages\"\n"
                            "  set targetService to 1st service whose service type = iMessage\n"
                            "  set targetBuddy to buddy \"%s\" of targetService\n"
                            "  send POSIX file \"%s\" to targetBuddy\n"
                            "end tell",
                            m_tgt_esc, path_esc);
                        c->alloc->free(c->alloc->ctx, path_esc, path_esc_cap);
                        if (m_n > 0 && (size_t)m_n < m_script_cap) {
                            const char *argv[] = {"osascript", "-e", m_script, NULL};
                            hu_run_result_t result = {0};
                            hu_error_t err = hu_process_run(c->alloc, argv, NULL, 65536, &result);
                            bool ok = (err == HU_OK && result.success && result.exit_code == 0);
                            hu_run_result_free(c->alloc, &result);
                            if (!ok)
                                send_err = HU_ERR_CHANNEL_SEND;
                        }
                        c->alloc->free(c->alloc->ctx, m_script, m_script_cap);
                    } else {
                        c->alloc->free(c->alloc->ctx, path_esc, path_esc_cap);
                    }
                }
                c->alloc->free(c->alloc->ctx, m_tgt_esc, m_tgt_cap);
            }
        }
    }
#endif

imsg_cleanup:
    if (clean)
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
#elif HU_IS_TEST
    return true;
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
            HU_CODENAME);
        return false;
    }
    return true;
#endif
}

static hu_error_t imessage_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                                     const char *contact_id, size_t contact_id_len,
                                                     size_t limit, hu_channel_history_entry_t **out,
                                                     size_t *out_count) {
    (void)ctx;
    if (!alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_SUPPORTED;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_INTERNAL;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_INTERNAL;
    }

    const char *sql = "SELECT m.is_from_me, m.text, "
                      "  datetime(m.date/1000000000 + 978307200, 'unixepoch', 'localtime') as ts "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.associated_message_type = 0 "
                      "ORDER BY m.date DESC LIMIT ?2";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_INTERNAL;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, -1, NULL);
    sqlite3_bind_int(stmt, 2, (int)(limit > 50 ? 50 : limit));

    if (limit > 50)
        limit = 50;
    hu_channel_history_entry_t *entries =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(*entries));
    if (!entries) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, limit * sizeof(*entries));
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
        entries[count].from_me = sqlite3_column_int(stmt, 0) != 0;
        const char *txt = (const char *)sqlite3_column_text(stmt, 1);
        const char *ts = (const char *)sqlite3_column_text(stmt, 2);
        if (txt && strlen(txt) > 0) {
            size_t tlen = strlen(txt);
            if (tlen >= sizeof(entries[0].text))
                tlen = sizeof(entries[0].text) - 1;
            memcpy(entries[count].text, txt, tlen);
            entries[count].text[tlen] = '\0';
        } else if (entries[count].from_me) {
            snprintf(entries[count].text, sizeof(entries[0].text), "[you replied]");
        } else {
            /* Empty text = attachment. Use generic placeholder; prompt builder
             * should call hu_conversation_attachment_context() to inject guidance
             * for natural acknowledgment ("love that!", "that looks great", etc.). */
            snprintf(entries[count].text, sizeof(entries[0].text), "[image or attachment]");
        }
        if (ts) {
            size_t tslen = strlen(ts);
            if (tslen >= sizeof(entries[0].timestamp))
                tslen = sizeof(entries[0].timestamp) - 1;
            memcpy(entries[count].timestamp, ts, tslen);
            entries[count].timestamp[tslen] = '\0';
        }
        count++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Results come in DESC order; reverse to chronological for caller */
    for (size_t i = 0; i < count / 2; i++) {
        hu_channel_history_entry_t tmp = entries[i];
        entries[i] = entries[count - 1 - i];
        entries[count - 1 - i] = tmp;
    }

    *out = entries;
    *out_count = count;
    return HU_OK;
#else
    (void)contact_id_len;
    (void)limit;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t imessage_get_response_constraints(void *ctx,
                                                    hu_channel_response_constraints_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->max_chars = 300;
    return HU_OK;
}

static hu_error_t imessage_react(void *ctx, const char *target, size_t target_len,
                                 int64_t message_id, hu_reaction_type_t reaction) {
    (void)target_len;
    (void)message_id;
#if HU_IS_TEST
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->last_reaction = reaction;
    c->last_reaction_message_id = message_id;
    return HU_OK;
#else
#if !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)reaction;
    return HU_ERR_NOT_SUPPORTED;
#elif !defined(HU_IMESSAGE_TAPBACK_ENABLED)
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)reaction;
    if (getenv("HU_DEBUG"))
        fprintf(stderr, "[imessage] tapback disabled (HU_IMESSAGE_TAPBACK_ENABLED=OFF)\n");
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    const char *tapback_ax = imessage_reaction_to_ax_menu_name(reaction);
    if (!tapback_ax)
        return HU_ERR_INVALID_ARGUMENT;
    if (!target || target_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Fetch message content from chat.db for row matching (message_id > 0). */
    char content_buf[256];
    size_t content_len = 0;
#if defined(HU_ENABLE_SQLITE)
    if (message_id > 0) {
        const char *home_env = getenv("HOME");
        if (home_env) {
            char db_path[512];
            int dn = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home_env);
            if (dn > 0 && (size_t)dn < sizeof(db_path)) {
                sqlite3 *db = NULL;
                if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
                    sqlite3_stmt *stmt = NULL;
                    if (sqlite3_prepare_v2(db, "SELECT text FROM message WHERE ROWID = ?", -1,
                                           &stmt, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(stmt, 1, message_id);
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            const char *text = (const char *)sqlite3_column_text(stmt, 0);
                            if (text) {
                                size_t len = strlen(text);
                                if (len >= sizeof(content_buf))
                                    len = sizeof(content_buf) - 1;
                                memcpy(content_buf, text, len);
                                content_buf[len] = '\0';
                                content_len = len;
                            }
                        }
                        sqlite3_finalize(stmt);
                    }
                    sqlite3_close(db);
                }
            }
        }
    }
#endif
    (void)message_id;

    /* Escape content_prefix and tapback_ax for JavaScript string literals. */
    size_t esc_cap = (content_len + strlen(tapback_ax)) * 2 + 64;
    if (esc_cap > 2048)
        esc_cap = 2048;
    char *content_esc = (char *)c->alloc->alloc(c->alloc->ctx, esc_cap);
    if (!content_esc)
        return HU_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (size_t i = 0; i < content_len && j + 2 < esc_cap; i++) {
        if (content_buf[i] == '\\' || content_buf[i] == '"') {
            content_esc[j++] = '\\';
            content_esc[j++] = content_buf[i];
        } else if (content_buf[i] == '\n') {
            content_esc[j++] = '\\';
            content_esc[j++] = 'n';
        } else {
            content_esc[j++] = content_buf[i];
        }
    }
    content_esc[j] = '\0';
    size_t content_esc_len = j;

    size_t tapback_esc_cap = strlen(tapback_ax) * 2 + 16;
    char *tapback_esc = (char *)c->alloc->alloc(c->alloc->ctx, tapback_esc_cap);
    if (!tapback_esc) {
        c->alloc->free(c->alloc->ctx, content_esc, esc_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    j = 0;
    for (size_t i = 0; tapback_ax[i] && j + 2 < tapback_esc_cap; i++) {
        if (tapback_ax[i] == '\\' || tapback_ax[i] == '"') {
            tapback_esc[j++] = '\\';
            tapback_esc[j++] = tapback_ax[i];
        } else {
            tapback_esc[j++] = tapback_ax[i];
        }
    }
    tapback_esc[j] = '\0';

    /*
     * JXA script: activate Messages, find row by content, AXShowMenu, select tapback.
     * - contentPrefix: substring to match in table row (empty = use last row)
     * - tapbackAx: AX menu label (Love, Like, Ha Ha, etc.)
     * Fragile: requires accessibility permissions; UI hierarchy varies by macOS.
     */
    static const char *script_tpl =
        "ObjC.import(\"stdlib\");"
        "var contentPrefix=\"%s\";var tapbackAx=\"%s\";"
        "try{"
        "var M=Application(\"Messages\");M.activate();delay(0.5);"
        "var SE=Application(\"System Events\");var p=SE.processes[\"Messages\"];"
        "if(!p||!p.exists()){$.exit(1);}"
        "var w=p.windows();if(!w||w.length===0){$.exit(1);}"
        "var win=w[0];var t=win.tables();"
        "if(!t||t.length===0){$.exit(1);}"
        "var rows=t[t.length-1].rows();"
        "if(!rows||rows.length===0){$.exit(1);}"
        "var r=rows[rows.length-1];"
        "if(contentPrefix&&contentPrefix.length>0){"
        "for(var i=rows.length-1;i>=0;i--){"
        "var row=rows[i];var val=\"\";"
        "try{if(row.attributes&&row.attributes.AXValue!==undefined){val=String(row.attributes."
        "AXValue);}}catch(e){}"
        "try{if(!val&&row.cells&&row.cells.length>0){var "
        "c0=row.cells[0];if(c0.attributes&&c0.attributes.AXValue!==undefined){val=String(c0."
        "attributes.AXValue);}}}catch(e){}"
        "if(val&&val.indexOf(contentPrefix)!==-1){r=row;break;}"
        "}}"
        "if(r.actions&&r.actions.AXShowMenu){r.actions.AXShowMenu.perform();}"
        "delay(0.5);"
        "var ctxMenus=r.menus?r.menus():[];"
        "if(ctxMenus&&ctxMenus.length>0){"
        "var items=ctxMenus[0].menuItems?ctxMenus[0].menuItems():[];"
        "for(var ii=0;ii<items.length;ii++){"
        "var it=items[ii];var title=(it.title?it.title():\"\")+\"\";"
        "if(title.indexOf(\"Tapback\")!==-1){it.click();delay(0.3);"
        "var sub=it.menus?it.menus():[];"
        "if(sub&&sub.length>0){var subItems=sub[0].menuItems?sub[0].menuItems():[];"
        "for(var si=0;si<subItems.length;si++){var "
        "st=(subItems[si].title?subItems[si].title():\"\")+\"\";"
        "if(st===tapbackAx){subItems[si].click();break;}}"
        "}break;}"
        "}}"
        "}"
        "$.exit(0);"
        "}catch(e){$.exit(1);}";

    size_t script_cap = 1024 + content_esc_len + strlen(tapback_esc);
    char *script = (char *)c->alloc->alloc(c->alloc->ctx, script_cap);
    if (!script) {
        c->alloc->free(c->alloc->ctx, tapback_esc, tapback_esc_cap);
        c->alloc->free(c->alloc->ctx, content_esc, esc_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(script, script_cap, script_tpl, content_esc, tapback_esc);
    c->alloc->free(c->alloc->ctx, tapback_esc, tapback_esc_cap);
    c->alloc->free(c->alloc->ctx, content_esc, esc_cap);
    if (n < 0 || (size_t)n >= script_cap) {
        c->alloc->free(c->alloc->ctx, script, script_cap);
        return HU_ERR_INTERNAL;
    }

    /*
     * Run osascript with 15s timeout via perl alarm wrapper.
     * Avoids hangs if Messages.app is not running or accessibility is denied.
     */
    const char *argv[] = {
        "perl", "-e", "alarm 15; exec @ARGV", "osascript", "-l", "JavaScript", "-e", script, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(c->alloc, argv, NULL, 65536, &result);
    c->alloc->free(c->alloc->ctx, script, script_cap);
    if (err != HU_OK) {
        if (getenv("HU_DEBUG"))
            fprintf(stderr, "[imessage] tapback osascript failed: hu_process_run err=%d\n",
                    (int)err);
        hu_run_result_free(c->alloc, &result);
        return HU_ERR_NOT_SUPPORTED;
    }
    int exit_code = result.exit_code;
    bool ok = result.success && exit_code == 0;
    hu_run_result_free(c->alloc, &result);
    if (!ok) {
        if (getenv("HU_DEBUG"))
            fprintf(stderr,
                    "[imessage] tapback JXA failed (exit=%d, accessibility may be denied)\n",
                    exit_code);
        return HU_ERR_NOT_SUPPORTED;
    }
    return HU_OK;
#endif
#endif
}

static const hu_channel_vtable_t imessage_vtable = {
    .start = imessage_start,
    .stop = imessage_stop,
    .send = imessage_send,
    .name = imessage_name,
    .health_check = imessage_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
    .load_conversation_history = imessage_load_conversation_history,
    .get_response_constraints = imessage_get_response_constraints,
    .react = imessage_react,
};

hu_error_t hu_imessage_create(hu_allocator_t *alloc, const char *default_target,
                              size_t default_target_len, const char *const *allow_from,
                              size_t allow_from_count, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
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
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->default_target, default_target, default_target_len);
        c->default_target[default_target_len] = '\0';
        c->default_target_len = default_target_len;
    }
    /* Seed last_rowid to current max so we only pick up NEW messages.
     * HU_IMESSAGE_LOOKBACK env var: if set, look back N messages from max
     * to pick up recently missed messages on restart. */
#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
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
                    const char *lookback_env = getenv("HU_IMESSAGE_LOOKBACK");
                    if (lookback_env) {
                        long lb = strtol(lookback_env, NULL, 10);
                        if (lb > 0 && lb < 100 && c->last_rowid > lb)
                            c->last_rowid -= lb;
                    }
                    sqlite3_close(db);
                }
            }
        }
    }
#endif

    out->ctx = c;
    out->vtable = &imessage_vtable;
    return HU_OK;
}

bool hu_imessage_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    return c->default_target != NULL && c->default_target_len > 0;
}

void hu_imessage_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (c->default_target)
            free(c->default_target);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#ifndef HU_IS_TEST
#if defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
char *hu_imessage_get_attachment_path(hu_allocator_t *alloc, int64_t message_id) {
    if (!alloc || message_id <= 0)
        return NULL;

    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return NULL;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return NULL;
    }

    const char *sql = "SELECT a.filename FROM attachment a "
                      "JOIN message_attachment_join maj ON maj.attachment_id = a.ROWID "
                      "WHERE maj.message_id = ?1 LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_bind_int64(stmt, 1, message_id);

    char *path = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *filename = (const char *)sqlite3_column_text(stmt, 0);
        if (filename && filename[0]) {
            size_t len = strlen(filename);
            /* Expand ~ to home directory */
            if (len >= 1 && filename[0] == '~' && (len == 1 || filename[1] == '/')) {
                size_t home_len = strlen(home);
                size_t suffix_len = (len > 1) ? len - 1 : 0;
                size_t total = home_len + suffix_len + 1;
                path = (char *)alloc->alloc(alloc->ctx, total);
                if (path) {
                    memcpy(path, home, home_len);
                    if (suffix_len > 0)
                        memcpy(path + home_len, filename + 1, suffix_len);
                    path[total - 1] = '\0';
                }
            } else {
                path = hu_strndup(alloc, filename, len);
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    /* Validate path is within Messages attachments directory */
    if (path && home) {
        char allowed_prefix[512];
        int prefix_len = snprintf(allowed_prefix, sizeof(allowed_prefix),
                                  "%s/Library/Messages/Attachments/", home);
        if (prefix_len > 0 && (size_t)prefix_len < sizeof(allowed_prefix)) {
            if (strncmp(path, allowed_prefix, (size_t)prefix_len) != 0) {
                alloc->free(alloc->ctx, path, strlen(path) + 1);
                return NULL; /* Path outside allowed directory */
            }
        }
    }
    return path;
}
#else
char *hu_imessage_get_attachment_path(hu_allocator_t *alloc, int64_t message_id) {
    (void)alloc;
    (void)message_id;
    return NULL;
}
#endif

#if defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
char *hu_imessage_get_latest_attachment_path(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len) {
    if (!alloc || !contact_id || contact_id_len == 0)
        return NULL;

    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return NULL;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return NULL;
    }

    /* Get the most recent message with attachment from this contact */
    const char *sql = "SELECT a.filename FROM attachment a "
                      "JOIN message_attachment_join maj ON maj.attachment_id = a.ROWID "
                      "JOIN message m ON maj.message_id = m.ROWID "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 0 "
                      "ORDER BY m.date DESC LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    char contact_buf[256];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, -1, NULL);

    char *path = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *filename = (const char *)sqlite3_column_text(stmt, 0);
        if (filename && filename[0]) {
            size_t len = strlen(filename);
            if (len >= 1 && filename[0] == '~' && (len == 1 || filename[1] == '/')) {
                size_t home_len = strlen(home);
                size_t suffix_len = (len > 1) ? len - 1 : 0;
                size_t total = home_len + suffix_len + 1;
                path = (char *)alloc->alloc(alloc->ctx, total);
                if (path) {
                    memcpy(path, home, home_len);
                    if (suffix_len > 0)
                        memcpy(path + home_len, filename + 1, suffix_len);
                    path[total - 1] = '\0';
                }
            } else {
                path = hu_strndup(alloc, filename, len);
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    /* Validate path is within Messages attachments directory */
    if (path && home) {
        char allowed_prefix[512];
        int prefix_len = snprintf(allowed_prefix, sizeof(allowed_prefix),
                                  "%s/Library/Messages/Attachments/", home);
        if (prefix_len > 0 && (size_t)prefix_len < sizeof(allowed_prefix)) {
            if (strncmp(path, allowed_prefix, (size_t)prefix_len) != 0) {
                alloc->free(alloc->ctx, path, strlen(path) + 1);
                return NULL; /* Path outside allowed directory */
            }
        }
    }
    return path;
}
#else
char *hu_imessage_get_latest_attachment_path(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len) {
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    return NULL;
}
#endif
#endif

/* ── iMessage polling via ~/Library/Messages/chat.db ──────────────────── */

hu_error_t hu_imessage_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    (void)alloc;
    if (!channel_ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if HU_IS_TEST
    {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)channel_ctx;
        if (c->mock_count > 0) {
            size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
            for (size_t i = 0; i < n; i++) {
                memcpy(msgs[i].session_key, c->mock_msgs[i].session_key, 128);
                memcpy(msgs[i].content, c->mock_msgs[i].content, 4096);
                msgs[i].message_id = (int64_t)(i + 1);
                msgs[i].is_group = false;
                msgs[i].has_attachment = c->mock_msgs[i].has_attachment;
                msgs[i].has_video = c->mock_msgs[i].has_video;
                msgs[i].guid[0] = '\0';
            }
            *out_count = n;
            c->mock_count = 0;
            return HU_OK;
        }
        return HU_OK;
    }
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#elif !defined(HU_ENABLE_SQLITE)
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)channel_ctx;

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_SUPPORTED;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_INTERNAL;

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[imessage] cannot open chat.db: %s (rc=%d) — check Full Disk Access\n",
                db ? sqlite3_errmsg(db) : "unknown", rc);
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    /* If last_rowid was never seeded (e.g. FDA wasn't granted at startup),
     * seed it now to current max so we only pick up truly new messages. */
    if (c->last_rowid == 0) {
        sqlite3_stmt *seed = NULL;
        if (sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &seed, NULL) ==
            SQLITE_OK) {
            if (sqlite3_step(seed) == SQLITE_ROW)
                c->last_rowid = sqlite3_column_int64(seed, 0);
            sqlite3_finalize(seed);
        }
        fprintf(stderr,
                "[imessage] late-seeded last_rowid=%lld (only new messages will be processed)\n",
                (long long)c->last_rowid);
        sqlite3_close(db);
        *out_count = 0;
        return HU_OK;
    }

    /* Include text messages, photo-only, and video-only messages.
     * COALESCE: when text is NULL, use '[Video]' if video attachment else '[Photo]'.
     * has_image/has_video: subqueries check attachment table for extensions.
     * m.guid: message GUID for inline reply (associated_message_guid) tracking. */
    const char *sql =
        "SELECT m.ROWID, m.guid, "
        "  COALESCE(m.text, "
        "    (SELECT CASE "
        "       WHEN (SELECT COUNT(*) FROM message_attachment_join majv "
        "             JOIN attachment av ON majv.attachment_id = av.ROWID "
        "             WHERE majv.message_id = m.ROWID AND av.filename IS NOT NULL "
        "             AND (LOWER(av.filename) LIKE '%.mov' OR LOWER(av.filename) LIKE '%.mp4' "
        "               OR LOWER(av.filename) LIKE '%.m4v')) > 0 "
        "       THEN '[Video]' ELSE '[Photo]' END)) AS text, h.id, "
        "  COALESCE("
        "    (SELECT COUNT(DISTINCT chj2.handle_id) FROM chat_message_join cmj "
        "     JOIN chat_handle_join chj2 ON chj2.chat_id = cmj.chat_id "
        "     WHERE cmj.message_id = m.ROWID), 0) AS participant_count, "
        "  (SELECT COUNT(*) FROM message_attachment_join maj "
        "   JOIN attachment a ON maj.attachment_id = a.ROWID "
        "   WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "
        "   AND (LOWER(a.filename) LIKE '%.jpg' OR LOWER(a.filename) LIKE '%.jpeg' "
        "     OR LOWER(a.filename) LIKE '%.png' OR LOWER(a.filename) LIKE '%.heic' "
        "     OR LOWER(a.filename) LIKE '%.gif' OR LOWER(a.filename) LIKE '%.webp')) "
        "   > 0 AS has_image, "
        "  (SELECT COUNT(*) FROM message_attachment_join maj2 "
        "   JOIN attachment a2 ON maj2.attachment_id = a2.ROWID "
        "   WHERE maj2.message_id = m.ROWID AND a2.filename IS NOT NULL "
        "   AND (LOWER(a2.filename) LIKE '%.mov' OR LOWER(a2.filename) LIKE '%.mp4' "
        "     OR LOWER(a2.filename) LIKE '%.m4v')) > 0 AS has_video "
        "FROM message m "
        "JOIN handle h ON m.handle_id = h.ROWID "
        "WHERE m.is_from_me = 0 AND m.associated_message_type = 0 "
        "AND m.ROWID > ? "
        "AND ((m.text IS NOT NULL AND LENGTH(m.text) > 0) "
        "     OR (EXISTS (SELECT 1 FROM message_attachment_join maj "
        "         JOIN attachment a ON maj.attachment_id = a.ROWID "
        "         WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "
        "         AND ((LOWER(a.filename) LIKE '%.jpg' OR LOWER(a.filename) LIKE '%.jpeg' "
        "           OR LOWER(a.filename) LIKE '%.png' OR LOWER(a.filename) LIKE '%.heic' "
        "           OR LOWER(a.filename) LIKE '%.gif' OR LOWER(a.filename) LIKE '%.webp') "
        "           OR (LOWER(a.filename) LIKE '%.mov' OR LOWER(a.filename) LIKE '%.mp4' "
        "             OR LOWER(a.filename) LIKE '%.m4v'))))) "
        "ORDER BY m.ROWID ASC LIMIT ?";

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[imessage] SQL prepare failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, c->last_rowid);
    sqlite3_bind_int(stmt, 2, (int)max_msgs);

    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_msgs) {
        int64_t rowid = sqlite3_column_int64(stmt, 0);
        const char *guid = (const char *)sqlite3_column_text(stmt, 1);
        const char *text = (const char *)sqlite3_column_text(stmt, 2);
        const char *handle = (const char *)sqlite3_column_text(stmt, 3);
        int participant_count = sqlite3_column_int(stmt, 4);
        int has_image = sqlite3_column_int(stmt, 5);
        int has_video = sqlite3_column_int(stmt, 6);

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
                if (a[0] == '*' && a[1] == '\0') {
                    allowed = true;
                    break;
                }
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
        msgs[count].message_id = rowid;
        msgs[count].is_group = (participant_count > 2);
        msgs[count].has_attachment = (has_image != 0);
        msgs[count].has_video = (has_video != 0);
        if (guid && strlen(guid) > 0) {
            size_t g_len = strlen(guid);
            if (g_len >= sizeof(msgs[count].guid))
                g_len = sizeof(msgs[count].guid) - 1;
            memcpy(msgs[count].guid, guid, g_len);
            msgs[count].guid[g_len] = '\0';
        } else {
            msgs[count].guid[0] = '\0';
        }

        c->last_rowid = rowid;
        count++;
        if (getenv("HU_DEBUG"))
            fprintf(stderr, "[imessage] received from %.20s (group=%d): %.*s\n", handle,
                    (int)msgs[count - 1].is_group, (int)(text_len > 80 ? 80 : text_len), text);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (count == 0 && getenv("HU_DEBUG"))
        fprintf(stderr, "[imessage] poll: 0 messages (last_rowid=%lld)\n",
                (long long)c->last_rowid);

    *out_count = count;
    return HU_OK;
#endif
}

#if HU_IS_TEST
hu_error_t hu_imessage_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len) {
    return hu_imessage_test_inject_mock_ex(ch, session_key, session_key_len, content, content_len,
                                           false);
}

hu_error_t hu_imessage_test_inject_mock_ex(hu_channel_t *ch, const char *session_key,
                                           size_t session_key_len, const char *content,
                                           size_t content_len, bool has_attachment) {
    return hu_imessage_test_inject_mock_ex2(ch, session_key, session_key_len, content, content_len,
                                            has_attachment, false);
}

hu_error_t hu_imessage_test_inject_mock_ex2(hu_channel_t *ch, const char *session_key,
                                            size_t session_key_len, const char *content,
                                            size_t content_len, bool has_attachment,
                                            bool has_video) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    c->mock_msgs[i].has_attachment = has_attachment;
    c->mock_msgs[i].has_video = has_video;
    return HU_OK;
}

const char *hu_imessage_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}

void hu_imessage_test_get_last_reaction(hu_channel_t *ch, hu_reaction_type_t *out_reaction,
                                        int64_t *out_message_id) {
    if (!ch || !ch->ctx || !out_reaction || !out_message_id)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    *out_reaction = c->last_reaction;
    *out_message_id = c->last_reaction_message_id;
}
#endif
