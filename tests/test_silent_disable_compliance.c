/* tests/test_silent_disable_compliance.c
 *
 * Pins the contract from ~/.claude/rules/silent-config-gated-subsystems.md
 * for two subsystems Agent A owns:
 *   1. src/daemon_reaction_poll.c — reaction-collection polling
 *   2. src/security/audit.c       — security audit logger
 *
 * The rule says: when a config-gated subsystem is disabled, the first
 * call must emit ONE operator-visible log line naming the config key,
 * even if the call site fires 100+ times per process. The same is
 * expected for enable-confirmation lines.
 *
 * Strategy: redirect stderr to a temp file (matching the pattern in
 * tests/test_persona_filler_roundtrip.c), drive the subsystem, then
 * count matching lines in the captured buffer.
 */

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/daemon_reaction_poll.h"
#include "human/security/audit.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read entire file at path into out_buf (caller must free). Returns
 * bytes read. */
static size_t slurp_file(const char *path, char **out_buf) {
    *out_buf = NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return 0;
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    *out_buf = buf;
    return got;
}

/* Count occurrences of `needle` as a substring in `hay`. */
static int count_substr(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle)
        return 0;
    int n = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

/* Build a temp file path for redirecting stderr. */
static void make_tmp_path(char *out, size_t cap, const char *tag) {
    snprintf(out, cap, "/tmp/test_silent_disable_%s_%d.log", tag, (int)getpid());
}

static FILE *saved_stderr_for_restore(void) {
    /* Duplicate stderr fd so we can restore it after freopen. */
    int dup_fd = dup(fileno(stderr));
    if (dup_fd < 0)
        return NULL;
    return fdopen(dup_fd, "w");
}

static void restore_stderr(FILE *saved) {
    if (!saved)
        return;
    fflush(stderr);
    dup2(fileno(saved), fileno(stderr));
    fclose(saved);
}

/* ─────────────────────────────────────────────────────────────────────── */
/* reaction_poll: disabled config → exactly ONE log line, even for 100    */
/* repeated ticks.                                                        */
/* ─────────────────────────────────────────────────────────────────────── */

static void test_reaction_poll_disabled_emits_one_log_line_for_repeated_ticks(void) {
    hu_daemon_reaction_poll_reset_warn_guards_for_test();

    char tmp_path[256];
    make_tmp_path(tmp_path, sizeof(tmp_path), "rxn_disabled");

    FILE *saved = saved_stderr_for_restore();
    HU_ASSERT_NOT_NULL(saved);

    HU_ASSERT_NOT_NULL(freopen(tmp_path, "w+", stderr));

    /* Disabled config: enabled=false (the canonical disable signal). */
    hu_config_t cfg = {0};
    cfg.reaction_collection.enabled = false;

    /* Fire 100 ticks — rule says still exactly ONE log line. */
    for (int i = 0; i < 100; i++) {
        size_t ingested = 0;
        (void)hu_daemon_reaction_poll_tick(&cfg, 0, &ingested);
    }

    fflush(stderr);
    restore_stderr(saved);

    char *buf = NULL;
    (void)slurp_file(tmp_path, &buf);
    HU_ASSERT_NOT_NULL(buf);

    /* Must mention the config key the operator needs to flip. */
    HU_ASSERT_STR_CONTAINS(buf, "reaction_collection");
    HU_ASSERT_STR_CONTAINS(buf, "enabled=true");

    /* Must fire exactly ONCE across 100 ticks. We count a phrase that
     * appears EXACTLY once per emitted log line (not once per word). */
    int n = count_substr(buf, "disabled by config");
    HU_ASSERT_EQ(n, 1);

    free(buf);
    unlink(tmp_path);
}

/* ─────────────────────────────────────────────────────────────────────── */
/* reaction_poll: enabled config → exactly ONE positive-confirmation log  */
/* line.                                                                  */
/* ─────────────────────────────────────────────────────────────────────── */

static void test_reaction_poll_enabled_emits_one_positive_confirmation(void) {
    hu_daemon_reaction_poll_reset_warn_guards_for_test();

    char tmp_path[256];
    make_tmp_path(tmp_path, sizeof(tmp_path), "rxn_enabled");

    FILE *saved = saved_stderr_for_restore();
    HU_ASSERT_NOT_NULL(saved);
    HU_ASSERT_NOT_NULL(freopen(tmp_path, "w+", stderr));

    /* Enabled config: enabled=true. We don't set HU_CHATDB so the poll
     * tick will short-circuit after the enable warning fires. */
    hu_config_t cfg = {0};
    cfg.reaction_collection.enabled = true;
    /* channel_count=0 means "all channels", which includes imessage. */
    cfg.reaction_collection.channel_count = 0;
    /* Ensure HU_CHATDB is unset so we exit early after the enable line. */
    unsetenv("HU_CHATDB");

    /* Fire several ticks. */
    for (int i = 0; i < 5; i++) {
        size_t ingested = 0;
        (void)hu_daemon_reaction_poll_tick(&cfg, 0, &ingested);
    }

    fflush(stderr);
    restore_stderr(saved);

    char *buf = NULL;
    (void)slurp_file(tmp_path, &buf);
    HU_ASSERT_NOT_NULL(buf);

    /* Positive confirmation must say "enabled — polling". */
    HU_ASSERT_STR_CONTAINS(buf, "reaction_collection");
    HU_ASSERT_STR_CONTAINS(buf, "enabled");
    HU_ASSERT_STR_CONTAINS(buf, "polling");

    /* Exactly one positive-confirmation line across 5 ticks. */
    int n = count_substr(buf, "polling chat.db");
    HU_ASSERT_EQ(n, 1);

    free(buf);
    unlink(tmp_path);
}

/* ─────────────────────────────────────────────────────────────────────── */
/* audit_log: disabled config → exactly ONE log line, even when 50 events */
/* are submitted.                                                         */
/* ─────────────────────────────────────────────────────────────────────── */

static void test_audit_logger_disabled_emits_one_log_line(void) {
    hu_audit_logger_reset_warn_guards_for_test();

    char tmp_path[256];
    make_tmp_path(tmp_path, sizeof(tmp_path), "audit_disabled");

    FILE *saved = saved_stderr_for_restore();
    HU_ASSERT_NOT_NULL(saved);
    HU_ASSERT_NOT_NULL(freopen(tmp_path, "w+", stderr));

    /* Build a disabled audit logger using the system allocator. */
    hu_allocator_t alloc = hu_system_allocator();

    hu_audit_config_t cfg = {0};
    cfg.enabled = false;
    cfg.log_path = (char *)"audit.log";
    cfg.max_size_mb = 10;

    hu_audit_logger_t *logger = hu_audit_logger_create(&alloc, &cfg, "/tmp");
    HU_ASSERT_NOT_NULL(logger);

    /* Fire 50 events — rule says still exactly ONE log line. */
    for (int i = 0; i < 50; i++) {
        hu_audit_event_t ev;
        hu_audit_event_init(&ev, HU_AUDIT_COMMAND_EXECUTION);
        (void)hu_audit_logger_log(logger, &ev);
    }

    hu_audit_logger_destroy(logger, &alloc);

    fflush(stderr);
    restore_stderr(saved);

    char *buf = NULL;
    (void)slurp_file(tmp_path, &buf);
    HU_ASSERT_NOT_NULL(buf);

    /* Must mention the config key the operator needs to flip. */
    HU_ASSERT_STR_CONTAINS(buf, "audit");
    HU_ASSERT_STR_CONTAINS(buf, "audit.enabled=true");

    /* Must fire exactly ONCE across 50 attempted log calls. */
    int n = count_substr(buf, "audit logger disabled");
    HU_ASSERT_EQ(n, 1);

    free(buf);
    unlink(tmp_path);
}

void run_silent_disable_compliance_tests(void) {
    HU_TEST_SUITE("silent_disable_compliance");
    HU_RUN_TEST(test_reaction_poll_disabled_emits_one_log_line_for_repeated_ticks);
    HU_RUN_TEST(test_reaction_poll_enabled_emits_one_positive_confirmation);
    HU_RUN_TEST(test_audit_logger_disabled_emits_one_log_line);
}
