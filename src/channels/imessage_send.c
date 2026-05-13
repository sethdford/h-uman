/*
 * imessage_send.c — Text + media send (dual-path: imsg CLI / AppleScript).
 *
 * Step 6 of the iMessage shape refactor — see
 * docs/plans/2026-05-12-imessage-shape-refactor.md.
 *
 * Outbound message delivery. Two transport paths:
 *
 *   Path A — `imsg send` (preferred when use_imsg_cli + binary on $PATH).
 *            Subprocess invocation; faster, better error reporting,
 *            doesn't activate Messages.app.
 *   Path B — AppleScript (fallback). `osascript -e` with `tell
 *            application "Messages"`. Slower (~2-5 s); activates the
 *            Messages.app UI; older but widest macOS coverage.
 *
 * Media attachments follow text and use the same two-path strategy:
 *   `imsg send --file` first, AppleScript `send POSIX file …` fallback.
 *
 * `imessage_build_attach_script` is the AppleScript builder for the
 * fallback path. Kept in this file because send is its only caller.
 *
 * Pre-send pipeline:
 *   - Strip markdown (asterisks, headers, bullets, backticks).
 *   - Run imessage_sanitize_output (collapses double-spaces, strips
 *     trailing whitespace, applies hu_conversation_strip_ai_phrases).
 *   - Hard cap at 1000 chars, preferring sentence/word boundaries.
 *   - Fire imessage_simulate_typing for natural-feel delay (Tier-3 typing).
 *   - Track sent text in the sent-ring so poll doesn't echo our own send.
 */

#include "imessage_internal.h"

#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/process_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
#include <unistd.h>
#endif

#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST

/* Build an AppleScript to send a POSIX file attachment via iMessage.
 * Pure string builder — does NOT execute the script.
 * Returns bytes written (excluding NUL) or 0 on error. */
size_t imessage_build_attach_script(char *out, size_t out_cap, const char *target_escaped,
                                    const char *path_escaped) {
    if (!out || out_cap < 128 || !target_escaped || !path_escaped)
        return 0;
    int n = snprintf(out, out_cap,
                     "tell application \"Messages\"\n"
                     "  set targetService to 1st service whose service type = iMessage\n"
                     "  set targetBuddy to buddy \"%s\" of targetService\n"
                     "  send POSIX file \"%s\" to targetBuddy\n"
                     "end tell",
                     target_escaped, path_escaped);
    if (n > 0 && (size_t)n < out_cap)
        return (size_t)n;
    return 0;
}

#endif

hu_error_t imessage_send(void *ctx, const char *target, size_t target_len, const char *message,
                         size_t message_len, const char *const *media, size_t media_count) {
#if HU_IS_TEST
    (void)target;
    (void)target_len;
    {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
        if (!c)
            return HU_ERR_INVALID_ARGUMENT;
        /* Match production imessage_send validation (no AppleScript in tests). */
        if (message_len > 0 && !message)
            return HU_ERR_INVALID_ARGUMENT;
        if (message_len == 0 && media_count == 0)
            return HU_ERR_INVALID_ARGUMENT;
        c->last_media_count = media_count;
        if (media && media_count > 0 && media[0]) {
            size_t mp_len = strlen(media[0]);
            if (mp_len > sizeof(c->last_media_path) - 1)
                mp_len = sizeof(c->last_media_path) - 1;
            memcpy(c->last_media_path, media[0], mp_len);
            c->last_media_path[mp_len] = '\0';
        } else {
            c->last_media_path[0] = '\0';
        }
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        else
            len = 0;
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

        /* Hard length cap for iMessage: truncate at sentence boundary near 1000 chars */
        if (message_len > 1000) {
            size_t cut = 1000;
            while (cut > 200 && clean[cut] != '.' && clean[cut] != '!' && clean[cut] != '?')
                cut--;
            if (cut > 200) {
                message_len = cut + 1;
                clean[message_len] = '\0';
            } else {
                size_t space_cut = 1000;
                while (space_cut > 200 && clean[space_cut] != ' ')
                    space_cut--;
                if (space_cut > 200)
                    message_len = space_cut;
                else
                    message_len = 1000;
                clean[message_len] = '\0';
            }
        }

        imessage_simulate_typing(c, tgt, tgt_len, message_len);

        {
            if (c->use_imsg_cli && imsg_cli_available(c)) {
                char tgt_buf[256];
                size_t tb = tgt_len < sizeof(tgt_buf) - 1 ? tgt_len : sizeof(tgt_buf) - 1;
                memcpy(tgt_buf, tgt, tb);
                tgt_buf[tb] = '\0';
                const char *imsg_argv[] = {"imsg",  "send",      "--to",     tgt_buf, "--text",
                                           message, "--service", "imessage", NULL};
                hu_run_result_t imsg_result = {0};
                hu_error_t imsg_err =
                    hu_process_run_with_timeout(c->alloc, imsg_argv, NULL, 65536, 15, &imsg_result);
                bool imsg_ok =
                    (imsg_err == HU_OK && imsg_result.success && imsg_result.exit_code == 0);
                hu_run_result_free(c->alloc, &imsg_result);
                if (imsg_ok) {
                    imessage_record_sent(c, message, message_len);
                    goto imsg_media;
                }
                if (getenv("HU_DEBUG"))
                    hu_log_info("imessage", NULL, "imsg send failed, falling back to AppleScript");
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
imsg_media:
    /* Send media attachments (local file paths only) after text succeeds.
     * Prefer imsg send --file when available (faster, better error reporting);
     * fall back to AppleScript per-attachment on failure. */
    if (send_err == HU_OK && media && media_count > 0) {
        bool try_imsg_file = c->use_imsg_cli && imsg_cli_available(c);
        char imsg_tgt_buf[256];
        if (try_imsg_file) {
            size_t tb = tgt_len < sizeof(imsg_tgt_buf) - 1 ? tgt_len : sizeof(imsg_tgt_buf) - 1;
            memcpy(imsg_tgt_buf, tgt, tb);
            imsg_tgt_buf[tb] = '\0';
        }

        size_t m_tgt_cap = tgt_len * 2 + 1;
        char *m_tgt_esc = NULL;
        if (m_tgt_cap <= 4096) {
            m_tgt_esc = (char *)c->alloc->alloc(c->alloc->ctx, m_tgt_cap);
            if (m_tgt_esc)
                escape_for_applescript(m_tgt_esc, m_tgt_cap, tgt, tgt_len);
        }

        for (size_t i = 0; i < media_count && send_err == HU_OK; i++) {
            const char *url = media[i];
            if (!url || url[0] != '/')
                continue;
            if (access(url, R_OK) != 0)
                continue;

            if (try_imsg_file) {
                const char *fa[] = {"imsg", "send",      "--to",     imsg_tgt_buf, "--file",
                                    url,    "--service", "imessage", NULL};
                hu_run_result_t ir = {0};
                hu_error_t ie = hu_process_run_with_timeout(c->alloc, fa, NULL, 65536, 15, &ir);
                bool fok = (ie == HU_OK && ir.success && ir.exit_code == 0);
                hu_run_result_free(c->alloc, &ir);
                if (fok)
                    continue;
                if (getenv("HU_DEBUG"))
                    hu_log_info("imessage", NULL,
                                "imsg send --file failed, falling back to AppleScript");
            }

            if (!m_tgt_esc) {
                send_err = HU_ERR_CHANNEL_SEND;
                break;
            }
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
                size_t m_n =
                    imessage_build_attach_script(m_script, m_script_cap, m_tgt_esc, path_esc);
                c->alloc->free(c->alloc->ctx, path_esc, path_esc_cap);
                if (m_n > 0) {
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

        if (m_tgt_esc)
            c->alloc->free(c->alloc->ctx, m_tgt_esc, m_tgt_cap);
    }
#endif

imsg_cleanup:
    if (clean)
        c->alloc->free(c->alloc->ctx, clean, clean_cap);
    return send_err;
#endif
}
