#include "human/channels/imessage_action.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void emits_well_formed_jsonl_line(void) {
    /* Set env var to redirect log dir to a tmp path. */
    char tmpdir[] = "/tmp/human-tlm-XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(tmpdir));
    setenv("HU_IMESSAGE_ACTION_LOG_DIR", tmpdir, 1);

    hu_imessage_action_log_t log = {0};
    log.ts_unix = 1716681234;
    log.target_chat_id_hash = "a3f1deadbeef0000";
    log.facts.seconds_since_parent = 47;
    log.facts.conv_density_msgs_per_min = 3.2f;
    log.facts.parent_was_a_question = true;
    log.facts.persona_formality = 0.5f;
    log.facts.persona_thread_affinity = 0.3f;
    log.style_chosen = HU_REPLY_STYLE_THREADED;
    log.send_result = 0;
    log.tier_used = "ax_menu";
    log.elapsed_ms = 812;

    HU_ASSERT_EQ((int)hu_imessage_action_log_jsonl(&log), (int)HU_OK);

    char path[512];
    snprintf(path, sizeof(path), "%s/imessage_action.jsonl", tmpdir);
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[1024];
    HU_ASSERT_NOT_NULL(fgets(buf, sizeof(buf), f));
    fclose(f);

    HU_ASSERT(strstr(buf, "\"ts\":1716681234") != NULL);
    HU_ASSERT(strstr(buf, "\"chat\":\"a3f1deadbeef0000\"") != NULL);
    HU_ASSERT(strstr(buf, "\"style\":\"THREADED\"") != NULL);
    HU_ASSERT(strstr(buf, "\"tier\":\"ax_menu\"") != NULL);
    HU_ASSERT(strstr(buf, "\"elapsed_ms\":812") != NULL);
    HU_ASSERT(strstr(buf, "\"q\":true") != NULL);

    /* Clean up tmpdir + env. */
    unlink(path);
    rmdir(tmpdir);
    unsetenv("HU_IMESSAGE_ACTION_LOG_DIR");
}

static void appends_not_truncates(void) {
    char tmpdir[] = "/tmp/human-tlm-XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(tmpdir));
    setenv("HU_IMESSAGE_ACTION_LOG_DIR", tmpdir, 1);

    hu_imessage_action_log_t log = {0};
    log.tier_used = "cmdR";
    log.style_chosen = HU_REPLY_STYLE_FLAT;

    /* Write 3 entries. */
    log.ts_unix = 1;
    HU_ASSERT_EQ((int)hu_imessage_action_log_jsonl(&log), (int)HU_OK);
    log.ts_unix = 2;
    HU_ASSERT_EQ((int)hu_imessage_action_log_jsonl(&log), (int)HU_OK);
    log.ts_unix = 3;
    HU_ASSERT_EQ((int)hu_imessage_action_log_jsonl(&log), (int)HU_OK);

    char path[512];
    snprintf(path, sizeof(path), "%s/imessage_action.jsonl", tmpdir);
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    int line_count = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f))
        line_count++;
    fclose(f);
    HU_ASSERT_EQ(line_count, 3);

    unlink(path);
    rmdir(tmpdir);
    unsetenv("HU_IMESSAGE_ACTION_LOG_DIR");
}

void run_imessage_action_telemetry_tests(void) {
    HU_TEST_SUITE("imessage_action_telemetry");
    HU_RUN_TEST(emits_well_formed_jsonl_line);
    HU_RUN_TEST(appends_not_truncates);
}
