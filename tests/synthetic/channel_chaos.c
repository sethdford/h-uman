/*
 * Chaos testing layer for human channel harness.
 * Message chaos: unicode bomb, size extremes, rapid fire, interleave, malformed, echo storm.
 * Infrastructure chaos: double start/stop, null args, concurrent send.
 */
#include "channel_harness.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "synthetic_harness.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHAOS_CONTENT_MAX 4096
#define CHAOS_MOCK_BUF    8

/* ─── Message Chaos ──────────────────────────────────────────────────────── */

static void chaos_unicode_bomb(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                               const hu_channel_test_entry_t *entry, hu_synth_metrics_t *metrics) {
    (void)alloc;
    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        HU_CH_LOG("chaos_unicode_bomb: create failed for %s: %s", entry->name,
                  hu_error_string(err));
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return;
    }

    const char *session = "unicode_test";
    size_t sk_len = strlen(session);
    hu_synth_verdict_t verdict = HU_SYNTH_PASS;

    /* Zalgo text (H + combining acute, e + combining, l + combining, etc.) */
    static const unsigned char zalgo[] = {
        'H',  0xCC, 0x89, 'e',  0xCC, 0x8A, 0xCC, 0x8B, 'l',
        0xCC, 0x89, 'l',  0xCC, 0x8A, 'o',  0xCC, 0x89,
    };
    err = entry->inject(&ch, session, sk_len, (const char *)zalgo, sizeof(zalgo));
    if (err != HU_OK) {
        HU_CH_VERBOSE(cfg, "unicode_bomb zalgo inject failed: %s", hu_error_string(err));
        verdict = HU_SYNTH_FAIL;
    }

    /* RTL override (U+202E) */
    static const unsigned char rtl[] = {
        0xE2, 0x80, 0xAE, 'r', 'e', 'v', 'e', 'r', 's', 'e', 'd', ' ', 't', 'e', 'x', 't',
    };
    if (verdict == HU_SYNTH_PASS) {
        err = entry->inject(&ch, session, sk_len, (const char *)rtl, sizeof(rtl));
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    /* Emoji sequence (grinning, smile, etc.) */
    static const unsigned char emoji[] = {
        0xF0, 0x9F, 0x98, 0x80, 0xF0, 0x9F, 0x98, 0x81,
        0xF0, 0x9F, 0x98, 0x82, 0xF0, 0x9F, 0x98, 0x83,
    };
    if (verdict == HU_SYNTH_PASS) {
        err = entry->inject(&ch, session, sk_len, (const char *)emoji, sizeof(emoji));
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    /* Null bytes embedded */
    static const char null_embed[] = "hello\x00world";
    if (verdict == HU_SYNTH_PASS) {
        err = entry->inject(&ch, session, sk_len, null_embed, sizeof(null_embed) - 1);
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    /* Poll and send verify */
    if (verdict == HU_SYNTH_PASS && entry->poll) {
        hu_channel_loop_msg_t msgs[16];
        size_t count = 0;
        err = entry->poll(ch.ctx, alloc, msgs, 16, &count);
        if (err != HU_OK || count == 0)
            verdict = HU_SYNTH_FAIL;
    }

    if (verdict == HU_SYNTH_PASS && ch.vtable && ch.vtable->send) {
        err = ch.vtable->send(ch.ctx, session, sk_len, "test", 4, NULL, 0);
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    entry->destroy(&ch);
    hu_synth_metrics_record(alloc, metrics, 0, verdict);
    HU_CH_VERBOSE(cfg, "chaos_unicode_bomb %s: %s", entry->name, hu_synth_verdict_str(verdict));
}

static void chaos_size_extremes(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                const hu_channel_test_entry_t *entry, hu_synth_metrics_t *metrics) {
    (void)alloc;
    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return;
    }

    const char *session = "size_test";
    size_t sk_len = strlen(session);
    hu_synth_verdict_t verdict = HU_SYNTH_PASS;

    /* Empty message */
    err = entry->inject(&ch, session, sk_len, "", 0);
    if (err != HU_OK)
        verdict = HU_SYNTH_FAIL;

    /* Single char */
    if (verdict == HU_SYNTH_PASS) {
        err = entry->inject(&ch, session, sk_len, "x", 1);
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    /* Exactly 4095 bytes */
    if (verdict == HU_SYNTH_PASS) {
        char *buf4095 = (char *)alloc->alloc(alloc->ctx, 4096);
        if (!buf4095) {
            verdict = HU_SYNTH_ERROR;
        } else {
            memset(buf4095, 'a', 4095);
            buf4095[4095] = '\0';
            err = entry->inject(&ch, session, sk_len, buf4095, 4095);
            alloc->free(alloc->ctx, buf4095, 4096);
            if (err != HU_OK)
                verdict = HU_SYNTH_FAIL;
        }
    }

    /* 8000 bytes — should truncate, no overflow */
    if (verdict == HU_SYNTH_PASS) {
        char *buf8000 = (char *)alloc->alloc(alloc->ctx, 8001);
        if (!buf8000) {
            verdict = HU_SYNTH_ERROR;
        } else {
            memset(buf8000, 'b', 8000);
            buf8000[8000] = '\0';
            err = entry->inject(&ch, session, sk_len, buf8000, 8000);
            alloc->free(alloc->ctx, buf8000, 8001);
            if (err != HU_OK)
                verdict = HU_SYNTH_FAIL;
        }
    }

    /* Poll: truncated content, no buffer overflow */
    if (verdict == HU_SYNTH_PASS && entry->poll) {
        hu_channel_loop_msg_t msgs[16];
        size_t count = 0;
        err = entry->poll(ch.ctx, alloc, msgs, 16, &count);
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
        /* Content should not exceed CHAOS_CONTENT_MAX - 1 */
        for (size_t i = 0; i < count && verdict == HU_SYNTH_PASS; i++) {
            size_t len = strlen(msgs[i].content);
            if (len >= CHAOS_CONTENT_MAX)
                verdict = HU_SYNTH_FAIL;
        }
    }

    entry->destroy(&ch);
    hu_synth_metrics_record(alloc, metrics, 0, verdict);
    HU_CH_VERBOSE(cfg, "chaos_size_extremes %s: %s", entry->name, hu_synth_verdict_str(verdict));
}

static void chaos_rapid_fire(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                             const hu_channel_test_entry_t *entry, hu_synth_metrics_t *metrics) {
    (void)alloc;
    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return;
    }

    const char *session = "rapid_session";
    size_t sk_len = strlen(session);
    hu_synth_verdict_t verdict = HU_SYNTH_PASS;

    int ok_count = 0;
    for (int i = 0; i < 50; i++) {
        char msg[32];
        int n = snprintf(msg, sizeof(msg), "msg%d", i);
        if (n < 0 || (size_t)n >= sizeof(msg))
            continue;
        err = entry->inject(&ch, session, sk_len, msg, (size_t)n);
        if (err == HU_OK)
            ok_count++;
    }
    if (ok_count < 1 || ok_count > CHAOS_MOCK_BUF)
        verdict = HU_SYNTH_FAIL;

    if (verdict == HU_SYNTH_PASS && entry->poll) {
        hu_channel_loop_msg_t msgs[16];
        size_t count = 0;
        err = entry->poll(ch.ctx, alloc, msgs, 16, &count);
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
        /* Mock buffer should cap at 8; we expect at most 8 */
        if (count > CHAOS_MOCK_BUF)
            verdict = HU_SYNTH_FAIL;
    }

    entry->destroy(&ch);
    hu_synth_metrics_record(alloc, metrics, 0, verdict);
    HU_CH_VERBOSE(cfg, "chaos_rapid_fire %s: %s", entry->name, hu_synth_verdict_str(verdict));
}

static void chaos_interleave(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                             const hu_channel_test_entry_t *entry, hu_synth_metrics_t *metrics) {
    (void)alloc;
    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return;
    }

    static const char *sessions[] = {"sess_a", "sess_b", "sess_c", "sess_d", "sess_e"};
    static const char *msgs[] = {"msg_a", "msg_b", "msg_c", "msg_d", "msg_e"};
    const size_t n_sessions = sizeof(sessions) / sizeof(sessions[0]);

    /* Round-robin inject */
    for (size_t r = 0; r < 3; r++) {
        for (size_t i = 0; i < n_sessions; i++) {
            size_t sk_len = strlen(sessions[i]);
            size_t c_len = strlen(msgs[i]);
            err = entry->inject(&ch, sessions[i], sk_len, msgs[i], c_len);
            if (err != HU_OK) {
                hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_FAIL);
                entry->destroy(&ch);
                return;
            }
        }
    }

    hu_synth_verdict_t verdict = HU_SYNTH_PASS;
    if (entry->poll) {
        hu_channel_loop_msg_t msgs_out[32];
        size_t count = 0;
        err = entry->poll(ch.ctx, alloc, msgs_out, 32, &count);
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
        else {
            /* Verify session_key isolation */
            for (size_t i = 0; i < count && verdict == HU_SYNTH_PASS; i++) {
                bool found = false;
                for (size_t j = 0; j < n_sessions; j++) {
                    if (strcmp(msgs_out[i].session_key, sessions[j]) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    verdict = HU_SYNTH_FAIL;
            }
        }
    }

    entry->destroy(&ch);
    hu_synth_metrics_record(alloc, metrics, 0, verdict);
    HU_CH_VERBOSE(cfg, "chaos_interleave %s: %s", entry->name, hu_synth_verdict_str(verdict));
}

static void chaos_malformed(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                            const hu_channel_test_entry_t *entry, hu_synth_metrics_t *metrics) {
    (void)alloc;
    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return;
    }

    const char *session = "malformed";
    size_t sk_len = strlen(session);
    hu_synth_verdict_t verdict = HU_SYNTH_PASS;

    static const struct {
        const char *content;
        size_t len;
    } cases[] = {
        {"<script>alert('xss')</script>", 28},
        {"'; DROP TABLE messages; --", 25},
        {"{{{{{{{{{{", 10},
    };
    static const unsigned char invalid_utf8[] = {0xFF, 0xFE, 0xFD, 0xFC};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        err = entry->inject(&ch, session, sk_len, cases[i].content, cases[i].len);
        if (err != HU_OK) {
            verdict = HU_SYNTH_FAIL;
            break;
        }
    }
    if (verdict == HU_SYNTH_PASS) {
        err = entry->inject(&ch, session, sk_len, (const char *)invalid_utf8, sizeof(invalid_utf8));
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    if (verdict == HU_SYNTH_PASS && entry->poll) {
        hu_channel_loop_msg_t msgs[16];
        size_t count = 0;
        err = entry->poll(ch.ctx, alloc, msgs, 16, &count);
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    entry->destroy(&ch);
    hu_synth_metrics_record(alloc, metrics, 0, verdict);
    HU_CH_VERBOSE(cfg, "chaos_malformed %s: %s", entry->name, hu_synth_verdict_str(verdict));
}

static void chaos_echo_storm(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                             const hu_channel_test_entry_t *entry, hu_synth_metrics_t *metrics) {
    (void)alloc;
    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return;
    }

    const char *session = "echo_session";
    size_t sk_len = strlen(session);
    const char *content = "Echo test";
    size_t c_len = strlen(content);
    hu_synth_verdict_t verdict = HU_SYNTH_PASS;

    /* Send first */
    if (ch.vtable && ch.vtable->send) {
        err = ch.vtable->send(ch.ctx, session, sk_len, content, c_len, NULL, 0);
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    /* Inject same content back */
    if (verdict == HU_SYNTH_PASS) {
        err = entry->inject(&ch, session, sk_len, content, c_len);
        if (err != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    /* Poll and verify */
    if (verdict == HU_SYNTH_PASS && entry->poll) {
        hu_channel_loop_msg_t msgs[16];
        size_t count = 0;
        err = entry->poll(ch.ctx, alloc, msgs, 16, &count);
        if (err != HU_OK || count == 0)
            verdict = HU_SYNTH_FAIL;
        else if (strcmp(msgs[0].content, content) != 0)
            verdict = HU_SYNTH_FAIL;
    }

    entry->destroy(&ch);
    hu_synth_metrics_record(alloc, metrics, 0, verdict);
    HU_CH_VERBOSE(cfg, "chaos_echo_storm %s: %s", entry->name, hu_synth_verdict_str(verdict));
}

/* ─── Infrastructure Chaos ───────────────────────────────────────────────── */

static void chaos_double_start_stop(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                    const hu_channel_test_entry_t *entry,
                                    hu_synth_metrics_t *metrics) {
    (void)alloc;
    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return;
    }

    hu_synth_verdict_t verdict = HU_SYNTH_PASS;
    if (!ch.vtable || !ch.vtable->start || !ch.vtable->stop) {
        verdict = HU_SYNTH_SKIP;
    } else {
        err = ch.vtable->start(ch.ctx);
        if (err != HU_OK) {
            verdict = HU_SYNTH_FAIL;
        } else {
            hu_error_t err2 = ch.vtable->start(ch.ctx);
            if (err2 != HU_OK && err2 != HU_ERR_ALREADY_EXISTS)
                verdict = HU_SYNTH_FAIL;
            ch.vtable->stop(ch.ctx);
            ch.vtable->stop(ch.ctx);
        }
    }

    entry->destroy(&ch);
    hu_synth_metrics_record(alloc, metrics, 0, verdict);
    HU_CH_VERBOSE(cfg, "chaos_double_start_stop %s: %s", entry->name,
                  hu_synth_verdict_str(verdict));
}

static void chaos_null_args(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                            const hu_channel_test_entry_t *entry, hu_synth_metrics_t *metrics) {
    (void)alloc;
    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return;
    }

    hu_synth_verdict_t verdict = HU_SYNTH_PASS;

    /* inject(NULL, ...) - should return HU_ERR_INVALID_ARGUMENT */
    hu_error_t e = entry->inject(NULL, "sk", 2, "msg", 3);
    if (e != HU_ERR_INVALID_ARGUMENT)
        verdict = HU_SYNTH_FAIL;

    /* inject(&ch, NULL, 0, "msg", 3) - handle gracefully (error or accept) */
    if (verdict == HU_SYNTH_PASS) {
        e = entry->inject(&ch, NULL, 0, "msg", 3);
        if (e != HU_ERR_INVALID_ARGUMENT && e != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    /* poll with NULL ctx - should return error */
    if (verdict == HU_SYNTH_PASS && entry->poll) {
        hu_channel_loop_msg_t msgs[16];
        size_t count = 0;
        e = entry->poll(NULL, alloc, msgs, 16, &count);
        if (e != HU_ERR_INVALID_ARGUMENT && e != HU_OK)
            verdict = HU_SYNTH_FAIL;
    }

    entry->destroy(&ch);
    hu_synth_metrics_record(alloc, metrics, 0, verdict);
    HU_CH_VERBOSE(cfg, "chaos_null_args %s: %s", entry->name, hu_synth_verdict_str(verdict));
}

static void chaos_concurrent_send(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                  const hu_channel_test_entry_t *entry,
                                  hu_synth_metrics_t *metrics) {
    (void)alloc;
    hu_channel_t ch = {0};
    hu_error_t err = entry->create(alloc, &ch);
    if (err != HU_OK) {
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_ERROR);
        return;
    }

    if (!ch.vtable || !ch.vtable->send) {
        entry->destroy(&ch);
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_SKIP);
        return;
    }

    const char *session = "concurrent";
    size_t sk_len = strlen(session);
    const char *msg = "concurrent test";
    size_t msg_len = strlen(msg);
    hu_synth_verdict_t verdict = HU_SYNTH_PASS;

    pid_t pid = fork();
    if (pid < 0) {
        verdict = HU_SYNTH_ERROR;
    } else if (pid == 0) {
        /* Child */
        hu_error_t e = ch.vtable->send(ch.ctx, session, sk_len, msg, msg_len, NULL, 0);
        _exit(e == HU_OK ? 0 : 1);
    } else {
        /* Parent: send simultaneously */
        hu_error_t e = ch.vtable->send(ch.ctx, session, sk_len, msg, msg_len, NULL, 0);
        if (e != HU_OK)
            verdict = HU_SYNTH_FAIL;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0)
            verdict = HU_SYNTH_ERROR;
        else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            verdict = HU_SYNTH_FAIL;
    }

    entry->destroy(&ch);
    hu_synth_metrics_record(alloc, metrics, 0, verdict);
    HU_CH_VERBOSE(cfg, "chaos_concurrent_send %s: %s", entry->name, hu_synth_verdict_str(verdict));
}

/* ─── Orchestrator ───────────────────────────────────────────────────────── */

hu_error_t hu_channel_run_chaos(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics) {
    (void)gemini;
    hu_synth_metrics_init(metrics);
    size_t reg_count = 0;
    const hu_channel_test_entry_t *reg = hu_channel_test_registry(&reg_count);

    for (size_t ci = 0; ci < reg_count; ci++) {
        const hu_channel_test_entry_t *entry = &reg[ci];

        /* Filter by channel list */
        if (!cfg->all_channels) {
            bool found = false;
            for (size_t j = 0; j < cfg->channel_count; j++) {
                if (strcmp(cfg->channels[j], entry->name) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found)
                continue;
        }

        HU_CH_LOG("chaos testing channel: %s", entry->name);

        if (cfg->chaos & HU_CHAOS_MESSAGE) {
            chaos_unicode_bomb(alloc, cfg, entry, metrics);
            chaos_size_extremes(alloc, cfg, entry, metrics);
            chaos_rapid_fire(alloc, cfg, entry, metrics);
            chaos_interleave(alloc, cfg, entry, metrics);
            chaos_malformed(alloc, cfg, entry, metrics);
            chaos_echo_storm(alloc, cfg, entry, metrics);
        }
        if (cfg->chaos & HU_CHAOS_INFRA) {
            chaos_double_start_stop(alloc, cfg, entry, metrics);
            chaos_null_args(alloc, cfg, entry, metrics);
            chaos_concurrent_send(alloc, cfg, entry, metrics);
        }
    }
    return HU_OK;
}
