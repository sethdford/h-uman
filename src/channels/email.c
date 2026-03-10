#include "human/channels/email.h"
#include "human/core/process_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !HU_IS_TEST
#include <ctype.h>
#endif

#define HU_EMAIL_LAST_MSG_SIZE  4096
#define HU_EMAIL_MOCK_INBOX_MAX 8

#if !HU_IS_TEST
static const char *hu_email_find_header_ci(const char *hdr, size_t hdr_len, const char *name,
                                           size_t name_len) {
    for (size_t i = 0; i + name_len + 1 < hdr_len; i++) {
        if (i > 0 && hdr[i - 1] != '\n')
            continue;
        size_t j;
        for (j = 0; j < name_len; j++) {
            if (tolower((unsigned char)hdr[i + j]) != tolower((unsigned char)name[j]))
                break;
        }
        if (j == name_len && hdr[i + j] == ':') {
            size_t v = i + j + 1;
            while (v < hdr_len && hdr[v] == ' ')
                v++;
            return hdr + v;
        }
    }
    return NULL;
}
#endif

typedef struct hu_email_ctx {
    hu_allocator_t *alloc;
    char *smtp_host;
    size_t smtp_host_len;
    uint16_t smtp_port;
    char *smtp_user;
    size_t smtp_user_len;
    char *smtp_pass;
    size_t smtp_pass_len;
    char *imap_host;
    size_t imap_host_len;
    uint16_t imap_port;
    char *from_address;
    size_t from_len;
    bool running;
    unsigned int last_uid;
#if HU_IS_TEST
    char last_message[HU_EMAIL_LAST_MSG_SIZE];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_inbox[HU_EMAIL_MOCK_INBOX_MAX];
    size_t mock_inbox_count;
    size_t mock_inbox_read;
#endif
} hu_email_ctx_t;

static hu_error_t email_start(void *ctx) {
    hu_email_ctx_t *c = (hu_email_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void email_stop(void *ctx) {
    hu_email_ctx_t *c = (hu_email_ctx_t *)ctx;
    if (c)
        c->running = false;
}

#if !HU_IS_TEST
static const char *email_from(hu_email_ctx_t *c) {
    if (c->from_address && c->from_len > 0)
        return c->from_address;
    return "human@localhost";
}
#endif

static hu_error_t email_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
#if HU_IS_TEST
    hu_email_ctx_t *c = (hu_email_ctx_t *)ctx;
    if (c && message && message_len > 0) {
        size_t tlen = target_len > 0 && target ? (target_len > 255 ? 255 : target_len) : 0;
        const char *from =
            (c->from_address && c->from_len > 0) ? c->from_address : "human@localhost";
        int n = snprintf(c->last_message, HU_EMAIL_LAST_MSG_SIZE,
                         "From: %s\r\nTo: %.*s\r\nSubject: human\r\nContent-Type: text/plain; "
                         "charset=utf-8\r\n\r\n%.*s",
                         from, (int)tlen, (target && tlen > 0) ? target : "", (int)message_len,
                         message);
        if (n > 0 && (size_t)n < HU_EMAIL_LAST_MSG_SIZE) {
            c->last_message_len = (size_t)n;
        } else {
            size_t copy = message_len;
            if (copy >= HU_EMAIL_LAST_MSG_SIZE)
                copy = HU_EMAIL_LAST_MSG_SIZE - 1;
            memcpy(c->last_message, message, copy);
            c->last_message[copy] = '\0';
            c->last_message_len = copy;
        }
    }
    return HU_OK;
#else
    hu_email_ctx_t *c = (hu_email_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->smtp_host || c->smtp_host_len == 0)
        return HU_ERR_NOT_SUPPORTED;

    const char *from = email_from(c);

    /* Write email to temp file (MIME plain text) */
    char tmppath[] = "/tmp/hu_email_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0)
        return HU_ERR_IO;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmppath);
        return HU_ERR_IO;
    }
    int n = fprintf(f,
                    "From: %s\r\nTo: %.*s\r\nSubject: human\r\nContent-Type: text/plain; "
                    "charset=utf-8\r\n\r\n%.*s",
                    from, (int)target_len, target, (int)message_len, message);
    fclose(f);
    if (n < 0) {
        unlink(tmppath);
        return HU_ERR_IO;
    }

    /* Null-terminate target for argv */
    char *target_str = (char *)c->alloc->alloc(c->alloc->ctx, target_len + 1);
    if (!target_str) {
        unlink(tmppath);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(target_str, target, target_len);
    target_str[target_len] = '\0';

    char smtp_url[512];
    bool use_tls = (c->smtp_port == 465 || c->smtp_port == 587);
    snprintf(smtp_url, sizeof(smtp_url), "%s://%.*s:%u", use_tls ? "smtps" : "smtp",
             (int)c->smtp_host_len, c->smtp_host, c->smtp_port);

    /* Base args + optional auth (up to 15 args) */
    const char *argv[16];
    int ai = 0;
    argv[ai++] = "curl";
    argv[ai++] = "--url";
    argv[ai++] = smtp_url;
    argv[ai++] = "--mail-from";
    argv[ai++] = from;
    argv[ai++] = "--mail-rcpt";
    argv[ai++] = target_str;
    argv[ai++] = "--upload-file";
    argv[ai++] = tmppath;
    if (use_tls && c->smtp_port == 587) {
        argv[ai++] = "--ssl-reqd";
    }
    if (c->smtp_user && c->smtp_user_len > 0) {
        argv[ai++] = "--user";
        /* user:pass stored as single string */
        argv[ai++] = c->smtp_user;
    }
    argv[ai] = NULL;

    hu_run_result_t run = {0};
    hu_error_t err = hu_process_run(c->alloc, argv, NULL, 4096, &run);
    c->alloc->free(c->alloc->ctx, target_str, target_len + 1);
    unlink(tmppath);
    hu_run_result_free(c->alloc, &run);
    return (err == HU_OK && run.success) ? HU_OK : HU_ERR_IO;
#endif
}

static const char *email_name(void *ctx) {
    (void)ctx;
    return "email";
}
static bool email_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t email_vtable = {
    .start = email_start,
    .stop = email_stop,
    .send = email_send,
    .name = email_name,
    .health_check = email_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_email_create(hu_allocator_t *alloc, const char *smtp_host, size_t smtp_host_len,
                           uint16_t smtp_port, const char *from_address, size_t from_len,
                           hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_email_ctx_t *c = (hu_email_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (smtp_host && smtp_host_len > 0) {
        c->smtp_host = (char *)alloc->alloc(alloc->ctx, smtp_host_len + 1);
        if (!c->smtp_host) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->smtp_host, smtp_host, smtp_host_len);
        c->smtp_host[smtp_host_len] = '\0';
        c->smtp_host_len = smtp_host_len;
    }
    c->smtp_port = smtp_port > 0 ? smtp_port : 587;
    if (from_address && from_len > 0) {
        c->from_address = (char *)alloc->alloc(alloc->ctx, from_len + 1);
        if (!c->from_address) {
            if (c->smtp_host)
                alloc->free(alloc->ctx, c->smtp_host, c->smtp_host_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->from_address, from_address, from_len);
        c->from_address[from_len] = '\0';
        c->from_len = from_len;
    }
    out->ctx = c;
    out->vtable = &email_vtable;
    return HU_OK;
}

hu_error_t hu_email_set_auth(hu_channel_t *ch, const char *user, size_t user_len, const char *pass,
                             size_t pass_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_email_ctx_t *c = (hu_email_ctx_t *)ch->ctx;
    if (user && user_len > 0 && pass && pass_len > 0) {
        /* Store as "user:pass" for curl --user */
        size_t combo_len = user_len + 1 + pass_len;
        c->smtp_user = (char *)malloc(combo_len + 1);
        if (!c->smtp_user)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(c->smtp_user, user, user_len);
        c->smtp_user[user_len] = ':';
        memcpy(c->smtp_user + user_len + 1, pass, pass_len);
        c->smtp_user[combo_len] = '\0';
        c->smtp_user_len = combo_len;
    }
    return HU_OK;
}

hu_error_t hu_email_set_imap(hu_channel_t *ch, const char *imap_host, size_t imap_host_len,
                             uint16_t imap_port) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_email_ctx_t *c = (hu_email_ctx_t *)ch->ctx;
    if (imap_host && imap_host_len > 0) {
        c->imap_host = (char *)malloc(imap_host_len + 1);
        if (!c->imap_host)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(c->imap_host, imap_host, imap_host_len);
        c->imap_host[imap_host_len] = '\0';
        c->imap_host_len = imap_host_len;
    }
    c->imap_port = imap_port > 0 ? imap_port : 993;
    return HU_OK;
}

void hu_email_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_email_ctx_t *c = (hu_email_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (a) {
            if (c->smtp_host)
                a->free(a->ctx, c->smtp_host, c->smtp_host_len + 1);
            if (c->smtp_user)
                a->free(a->ctx, c->smtp_user, c->smtp_user_len + 1);
            if (c->smtp_pass)
                a->free(a->ctx, c->smtp_pass, c->smtp_pass_len + 1);
            if (c->imap_host)
                a->free(a->ctx, c->imap_host, c->imap_host_len + 1);
            if (c->from_address)
                a->free(a->ctx, c->from_address, c->from_len + 1);
            a->free(a->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

bool hu_email_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_email_ctx_t *c = (hu_email_ctx_t *)ch->ctx;
    return c->smtp_host != NULL && c->smtp_host[0] != '\0' && c->from_address != NULL &&
           c->from_address[0] != '\0';
}

#if HU_IS_TEST
const char *hu_email_test_last_message(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_email_ctx_t *c = (hu_email_ctx_t *)ch->ctx;
    return c->last_message_len > 0 ? c->last_message : NULL;
}

hu_error_t hu_email_test_inject_mock_email(hu_channel_t *ch, const char *from, size_t from_len,
                                           const char *subject_or_body, size_t body_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_email_ctx_t *c = (hu_email_ctx_t *)ch->ctx;
    if (c->mock_inbox_count >= HU_EMAIL_MOCK_INBOX_MAX)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_inbox_count++;
    size_t sk = from_len > 127 ? 127 : from_len;
    size_t ct = body_len > 4095 ? 4095 : body_len;
    if (from && sk > 0)
        memcpy(c->mock_inbox[i].session_key, from, sk);
    c->mock_inbox[i].session_key[sk] = '\0';
    if (subject_or_body && ct > 0)
        memcpy(c->mock_inbox[i].content, subject_or_body, ct);
    c->mock_inbox[i].content[ct] = '\0';
    return HU_OK;
}
#endif

/* ── Email IMAP polling via curl ──────────────────────────────────────── */

hu_error_t hu_email_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count) {
    if (!channel_ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if HU_IS_TEST
    (void)alloc;
    hu_email_ctx_t *c = (hu_email_ctx_t *)channel_ctx;
    while (c->mock_inbox_read < c->mock_inbox_count && *out_count < max_msgs) {
        size_t i = c->mock_inbox_read++;
        size_t sk_len = strlen(c->mock_inbox[i].session_key);
        size_t ct_len = strlen(c->mock_inbox[i].content);
        if (sk_len >= sizeof(msgs[*out_count].session_key))
            sk_len = sizeof(msgs[*out_count].session_key) - 1;
        if (ct_len >= sizeof(msgs[*out_count].content))
            ct_len = sizeof(msgs[*out_count].content) - 1;
        memcpy(msgs[*out_count].session_key, c->mock_inbox[i].session_key, sk_len);
        msgs[*out_count].session_key[sk_len] = '\0';
        memcpy(msgs[*out_count].content, c->mock_inbox[i].content, ct_len);
        msgs[*out_count].content[ct_len] = '\0';
        (*out_count)++;
    }
    return HU_OK;
#else
    hu_email_ctx_t *c = (hu_email_ctx_t *)channel_ctx;
    if (!c->imap_host || c->imap_host_len == 0)
        return HU_ERR_NOT_SUPPORTED;
    if (!c->smtp_user || c->smtp_user_len == 0)
        return HU_ERR_NOT_SUPPORTED;

    /*
     * Use curl to fetch unseen messages via IMAP SEARCH.
     * curl "imaps://host:port/INBOX" -u user:pass -X "SEARCH UNSEEN"
     * Returns message sequence numbers, then we FETCH each one.
     *
     * Step 1: SEARCH UNSEEN to get message UIDs
     */
    char imap_url[512];
    snprintf(imap_url, sizeof(imap_url), "imaps://%.*s:%u/INBOX", (int)c->imap_host_len,
             c->imap_host, c->imap_port);

    char uid_search[64];
    if (c->last_uid > 0)
        snprintf(uid_search, sizeof(uid_search), "UID SEARCH UNSEEN UID %u:*", c->last_uid + 1);
    else
        snprintf(uid_search, sizeof(uid_search), "SEARCH UNSEEN");

    const char *search_argv[] = {"curl",       "--silent", "--url",    imap_url, "--user",
                                 c->smtp_user, "-X",       uid_search, NULL};

    hu_run_result_t search_result = {0};
    hu_error_t err = hu_process_run(alloc, search_argv, NULL, 8192, &search_result);
    if (err != HU_OK || !search_result.success) {
        hu_run_result_free(alloc, &search_result);
        return err != HU_OK ? err : HU_ERR_IO;
    }

    /* Parse SEARCH response: "* SEARCH 1 2 3\r\n" */
    const char *output = search_result.stdout_buf;
    if (!output || search_result.stdout_len == 0) {
        hu_run_result_free(alloc, &search_result);
        return HU_OK;
    }

    unsigned int uids[32];
    size_t uid_count = 0;
    const char *p = strstr(output, "* SEARCH");
    if (p) {
        p += 8;
        while (*p && uid_count < 32 && uid_count < max_msgs) {
            while (*p == ' ')
                p++;
            if (*p >= '0' && *p <= '9') {
                unsigned int uid = 0;
                while (*p >= '0' && *p <= '9') {
                    if (uid > (unsigned int)(UINT32_MAX / 10))
                        break;
                    uid = uid * 10 + (unsigned int)(*p - '0');
                    p++;
                }
                if (uid > c->last_uid) {
                    uids[uid_count++] = uid;
                }
            } else {
                break;
            }
        }
    }
    hu_run_result_free(alloc, &search_result);

    /* Step 2: FETCH each message (headers + body preview) */
    size_t count = 0;
    for (size_t i = 0; i < uid_count && count < max_msgs; i++) {
        char fetch_url[640];
        snprintf(fetch_url, sizeof(fetch_url),
                 "imaps://%.*s:%u/INBOX;UID=%u;SECTION=HEADER.FIELDS%%20(FROM%%20SUBJECT)",
                 (int)c->imap_host_len, c->imap_host, c->imap_port, uids[i]);

        const char *fetch_argv[] = {"curl",   "--silent",   "--url", fetch_url,
                                    "--user", c->smtp_user, NULL};

        hu_run_result_t fetch_result = {0};
        err = hu_process_run(alloc, fetch_argv, NULL, 8192, &fetch_result);
        if (err != HU_OK || !fetch_result.success) {
            hu_run_result_free(alloc, &fetch_result);
            continue;
        }

        /* Extract From: and Subject: from headers (case-insensitive, bounded) */
        const char *from_line = NULL;
        const char *subj_line = NULL;
        const char *hdr = fetch_result.stdout_buf;
        size_t hdr_len = fetch_result.stdout_len;
        if (hdr && hdr_len > 0) {
            from_line = hu_email_find_header_ci(hdr, hdr_len, "From", 4);
            subj_line = hu_email_find_header_ci(hdr, hdr_len, "Subject", 7);
        }

        /* session_key = sender address, content = subject */
        char session[128] = "unknown";
        char content[4096] = "";
        if (from_line) {
            size_t remaining = hdr_len - (size_t)(from_line - hdr);
            while (remaining > 0 && *from_line == ' ') {
                from_line++;
                remaining--;
            }
            size_t flen = 0;
            while (flen < remaining && from_line[flen] != '\r' && from_line[flen] != '\n' &&
                   flen < sizeof(session) - 1)
                flen++;
            memcpy(session, from_line, flen);
            session[flen] = '\0';
        }
        if (subj_line) {
            size_t remaining = hdr_len - (size_t)(subj_line - hdr);
            while (remaining > 0 && *subj_line == ' ') {
                subj_line++;
                remaining--;
            }
            size_t slen = 0;
            while (slen < remaining && subj_line[slen] != '\r' && subj_line[slen] != '\n' &&
                   slen < sizeof(content) - 1)
                slen++;
            memcpy(content, subj_line, slen);
            content[slen] = '\0';
        }
        hu_run_result_free(alloc, &fetch_result);

        /* Now fetch the body text */
        char body_url[640];
        snprintf(body_url, sizeof(body_url), "imaps://%.*s:%u/INBOX;UID=%u;SECTION=TEXT",
                 (int)c->imap_host_len, c->imap_host, c->imap_port, uids[i]);

        const char *body_argv[] = {"curl",   "--silent",   "--url", body_url,
                                   "--user", c->smtp_user, NULL};

        hu_run_result_t body_result = {0};
        err = hu_process_run(alloc, body_argv, NULL, 8192, &body_result);
        if (err == HU_OK && body_result.success && body_result.stdout_buf) {
            size_t ct_len = strlen(content);
            if (ct_len + 3 < sizeof(content)) {
                size_t blen = body_result.stdout_len;
                size_t avail = sizeof(content) - ct_len - 3;
                if (blen > avail)
                    blen = avail;
                if (blen > 0) {
                    if (ct_len > 0) {
                        memcpy(content + ct_len, "\n\n", 2);
                        ct_len += 2;
                    }
                    memcpy(content + ct_len, body_result.stdout_buf, blen);
                    content[ct_len + blen] = '\0';
                }
            }
        }
        hu_run_result_free(alloc, &body_result);

        size_t sk_len = strlen(session);
        size_t ct_len = strlen(content);
        if (sk_len >= sizeof(msgs[count].session_key))
            sk_len = sizeof(msgs[count].session_key) - 1;
        if (ct_len >= sizeof(msgs[count].content))
            ct_len = sizeof(msgs[count].content) - 1;
        memcpy(msgs[count].session_key, session, sk_len);
        msgs[count].session_key[sk_len] = '\0';
        memcpy(msgs[count].content, content, ct_len);
        msgs[count].content[ct_len] = '\0';

        c->last_uid = uids[i];
        count++;
    }

    *out_count = count;
    return HU_OK;
#endif
}
