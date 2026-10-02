/* test_cli_replay.c — `human replay`'s pure parts: argument parsing, the
 * live-state isolation check (memory.db is never opened), turn parsing and the
 * result row. Hermetic: temp dirs only, HOME/HU_STATE_DIR restored. */
#include "human/cli_replay.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include "test_tmpdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cli_replay_parse_requires_in_and_out(void) {
    hu_cli_replay_args_t a;
    char why[128];
    char *argv1[] = {"human", "replay", "--in", "t.jsonl"};
    HU_ASSERT_FALSE(hu_cli_replay_parse(4, argv1, &a, why, sizeof(why)));
    char *argv2[] = {"human", "replay", "--in", "t.jsonl", "--out", "o.jsonl", "--bogus", "x"};
    HU_ASSERT_FALSE(hu_cli_replay_parse(8, argv2, &a, why, sizeof(why)));
    char *argv3[] = {"human",         "replay",  "--in", "t.jsonl",       "--out",
                     "o.jsonl",       "--arm",   "live", "--temperature", "0",
                     "--no-director", "--limit", "40",   "--delay-ms",    "2000"};
    HU_ASSERT_TRUE(hu_cli_replay_parse(15, argv3, &a, why, sizeof(why)));
    HU_ASSERT_STR_EQ(a.arm, "live");
    HU_ASSERT_STR_EQ(a.endpoint, "http://127.0.0.1:8741/v1"); /* default */
    HU_ASSERT_TRUE(a.force_temperature);
    HU_ASSERT_FALSE(a.director);
    HU_ASSERT_EQ(a.limit, 40);
    HU_ASSERT_EQ(a.delay_ms, 2000);
}

static void cli_replay_provider_allowlist_is_local_only(void) {
    HU_ASSERT_TRUE(hu_cli_replay_provider_allowed("mlx_local"));
    HU_ASSERT_TRUE(hu_cli_replay_provider_allowed("ollama"));
    HU_ASSERT_FALSE(hu_cli_replay_provider_allowed("gemini"));
    HU_ASSERT_FALSE(hu_cli_replay_provider_allowed("openai"));
    HU_ASSERT_FALSE(hu_cli_replay_provider_allowed("anthropic"));
    HU_ASSERT_FALSE(hu_cli_replay_provider_allowed(NULL));
    hu_cli_replay_args_t a;
    char why[128];
    char *argv[] = {"human", "replay", "--in", "t", "--out", "o", "--provider", "gemini"};
    HU_ASSERT_FALSE(hu_cli_replay_parse(8, argv, &a, why, sizeof(why)));
    HU_ASSERT_STR_CONTAINS(why, "provider");
}

typedef struct env_save {
    char home[1024], state[1024], mem[1024];
    bool had_home, had_state, had_mem;
} env_save_t;

static void env_take(env_save_t *s) {
    const char *h = getenv("HOME"), *st = getenv("HU_STATE_DIR"),
               *m = getenv("HU_MEMORY_SQLITE_PATH");
    s->had_home = h != NULL;
    s->had_state = st != NULL;
    s->had_mem = m != NULL;
    snprintf(s->home, sizeof(s->home), "%s", h ? h : "");
    snprintf(s->state, sizeof(s->state), "%s", st ? st : "");
    snprintf(s->mem, sizeof(s->mem), "%s", m ? m : "");
}

static void env_put(const char *k, bool had, const char *v) {
    if (had)
        setenv(k, v, 1);
    else
        unsetenv(k);
}

static void env_restore(const env_save_t *s) {
    env_put("HOME", s->had_home, s->home);
    env_put("HU_STATE_DIR", s->had_state, s->state);
    env_put("HU_MEMORY_SQLITE_PATH", s->had_mem, s->mem);
}

static void touch_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

static void cli_replay_isolation_refuses_the_live_state(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char root[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_replay_iso", root, sizeof(root)));
    char live[600], live_mem[700], run[600], run_mem[700];
    snprintf(live, sizeof(live), "%s/.human", root);
    snprintf(live_mem, sizeof(live_mem), "%s/memory.db", live);
    snprintf(run, sizeof(run), "%s/run", root);
    snprintf(run_mem, sizeof(run_mem), "%s/memory.db", run);
    HU_ASSERT_EQ(mkdir(live, 0700), 0);
    HU_ASSERT_EQ(mkdir(run, 0700), 0);
    touch_file(live_mem);
    touch_file(run_mem);
    env_save_t saved;
    env_take(&saved);
    setenv("HOME", root, 1);
    char why[160];

    unsetenv("HU_STATE_DIR");
    unsetenv("HU_MEMORY_SQLITE_PATH");
    bool unset_ok = hu_cli_replay_isolation_ok(&alloc, why, sizeof(why));

    setenv("HU_STATE_DIR", live, 1); /* the live ~/.human itself */
    setenv("HU_MEMORY_SQLITE_PATH", run_mem, 1);
    bool live_state_ok = hu_cli_replay_isolation_ok(&alloc, why, sizeof(why));

    setenv("HU_STATE_DIR", run, 1);
    setenv("HU_MEMORY_SQLITE_PATH", live_mem, 1); /* the live memory.db */
    bool live_mem_ok = hu_cli_replay_isolation_ok(&alloc, why, sizeof(why));
    bool why_names_memory = strstr(why, "HU_MEMORY_SQLITE_PATH") != NULL;

    setenv("HU_MEMORY_SQLITE_PATH", run_mem, 1); /* a private copy */
    bool copy_ok = hu_cli_replay_isolation_ok(&alloc, why, sizeof(why));

    env_restore(&saved);
    hu_test_rm_rf(root);
    HU_ASSERT_FALSE(unset_ok);
    HU_ASSERT_FALSE(live_state_ok);
    HU_ASSERT_FALSE(live_mem_ok);
    HU_ASSERT_TRUE(why_names_memory);
    HU_ASSERT_TRUE(copy_ok);
}

static void cli_replay_parse_turn_joins_bubbles_and_history(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *line = "{\"id\":\"t7\",\"contact_id\":\"+15550002222\",\"ts\":1790000000,"
                       "\"inbound_bubbles\":[\"you up\",\"need a favor\"],"
                       "\"history\":[{\"from_me\":false,\"text\":\"hey\",\"ts\":\"2026-09-30 "
                       "10:00:00\"},{\"from_me\":true,\"text\":\"yo\"}]}";
    hu_cli_replay_turn_t t;
    HU_ASSERT_EQ(hu_cli_replay_parse_turn(&alloc, line, strlen(line), &t), HU_OK);
    HU_ASSERT_STR_EQ(t.id, "t7");
    HU_ASSERT_STR_EQ(t.contact_id, "+15550002222");
    HU_ASSERT_STR_EQ(t.inbound, "you up\nneed a favor");
    HU_ASSERT_EQ(t.history_count, 2);
    HU_ASSERT_FALSE(t.history[0].from_me);
    HU_ASSERT_TRUE(t.history[1].from_me);
    HU_ASSERT_STR_EQ(t.history[0].timestamp, "2026-09-30 10:00:00");
    hu_cli_replay_turn_free(&alloc, &t);

    const char *bad = "{\"id\":\"t8\",\"contact_id\":\"x\",\"inbound_bubbles\":[]}";
    HU_ASSERT_EQ(hu_cli_replay_parse_turn(&alloc, bad, strlen(bad), &t), HU_ERR_PARSE);
    HU_ASSERT_NULL(t.id);
}

static void cli_replay_result_row_has_bubbles_and_no_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_replay_turn_result_t r;
    memset(&r, 0, sizeof(r));
    r.action = HU_REPLAY_ACTION_TEXT;
    char b0[] = "yeah";
    char b1[] = "what time";
    r.bubbles[0] = b0;
    r.bubble_lens[0] = 4;
    r.bubbles[1] = b1;
    r.bubble_lens[1] = 9;
    r.bubble_count = 2;
    r.reply_calls = 1;
    r.reply_fp = 0xabcdefULL;
    char *row = NULL;
    size_t row_len = 0;
    HU_ASSERT_EQ(hu_cli_replay_format_result(&alloc, "t1", "live", 12, &r, &row, &row_len), HU_OK);
    HU_ASSERT_NOT_NULL(row);
    HU_ASSERT_STR_CONTAINS(row, "\"bubbles\":[\"yeah\",\"what time\"]");
    HU_ASSERT_STR_CONTAINS(row, "\"action\":\"text\"");
    HU_ASSERT_STR_CONTAINS(row, "\"reply_fp\":\"0000000000abcdef\"");
    HU_ASSERT_STR_CONTAINS(row, "\"channel_outbound_calls\":0");
    HU_ASSERT_STR_NOT_CONTAINS(row, "contact");
    alloc.free(alloc.ctx, row, row_len + 1);
}

void run_cli_replay_tests(void) {
    HU_TEST_SUITE("cli_replay");
    HU_RUN_TEST(cli_replay_parse_requires_in_and_out);
    HU_RUN_TEST(cli_replay_provider_allowlist_is_local_only);
    HU_RUN_TEST(cli_replay_isolation_refuses_the_live_state);
    HU_RUN_TEST(cli_replay_parse_turn_joins_bubbles_and_history);
    HU_RUN_TEST(cli_replay_result_row_has_bubbles_and_no_contact);
}
