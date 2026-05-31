/* End-to-end proof of the production reaction→learn loop (in-process HUML path).
 *
 * Drives a real reaction stream through the SAME entrypoint the daemon uses
 * (hu_reaction_handler_handle_event), then exercises the full chain that
 * lora_training_runner runs in production:
 *
 *   reactions (single-sided rows)  →  hu_dpo_export_paired (zip)  →  DPO trainer.step
 *
 * This pins the gap that unit tests alone left open: a reaction genuinely
 * becomes a trainable preference pair and the trainer consumes it. Plain
 * hu_dpo_export drops every single-sided reaction row (asserted here as the
 * contrast), which is exactly why the paired export exists.
 */
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/ml/dpo.h"
#include "human/ml/rl_trainer.h"
#include "test_framework.h"
#include <string.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

static void reaction_stream_becomes_trainable_pairs_e2e(void) {
#if defined(HU_ENABLE_SQLITE) && defined(HU_ENABLE_ML)
    hu_allocator_t alloc = hu_system_allocator();
    /* Normally an in-memory DB. When HU_E2E_GOLD_DB names a path, persist the
     * reaction corpus there instead, so the gold-standard script
     * (scripts/e2e-reaction-to-adapter-gold.sh) can hand the SAME single-sided
     * reaction rows to `human ml dpo-train` and exercise hu_dpo_export_paired
     * inside the production CLI before the real MLX fuse. */
    const char *gold_db = getenv("HU_E2E_GOLD_DB");
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(gold_db && gold_db[0] ? gold_db : ":memory:", &db), SQLITE_OK);
    hu_dpo_collector_t col = {0};
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, db, 64, &col), HU_OK);
    HU_ASSERT_EQ(hu_dpo_init_tables(&col), HU_OK);

    /* Same prompt across the stream; positive reactions praise `good`,
     * negative reactions pan `bad`. Both responses are >= 4 chars so they
     * survive the export corpus filter. */
    const char *prompt = "what should i ship first?";
    const char *good = "ship the small fix now";
    const char *bad = "rewrite the whole module";

    /* The reaction handler keeps GLOBAL message-lookup + per-turn state. Reset
     * it before the stream so this test is isolated and order-independent (it
     * may run after other suites that exercised the handler). */
    hu_reaction_handler_reset_for_test();
    hu_reaction_handler_set_collector(&col);
    for (int i = 0; i < 6; i++) {
        char thread[64], msg[64];
        snprintf(thread, sizeof(thread), "chat-%03d", i);
        snprintf(msg, sizeof(msg), "msg-%03d", i);
        bool positive = (i % 2 == 0);
        hu_reaction_handler_register_assistant_message_for_production(
            "imessage", thread, msg, prompt, positive ? good : bad);
        hu_reaction_event_t evt = {
            .channel_id = "imessage",
            .target_thread_id = thread,
            .target_message_ref = msg,
            .sender_handle = "+15550100001",
            .kind = positive ? HU_REACTION_LOVE : HU_REACTION_DISLIKE,
            .polarity = positive ? HU_REACTION_POSITIVE : HU_REACTION_NEGATIVE,
            .timestamp_unix = 1715472000 + i,
            .is_removal = 0,
        };
        (void)hu_reaction_handler_handle_event(&evt);
    }
    hu_reaction_handler_set_collector(NULL);
    /* Leave the handler's global state clean for any later suite. */
    hu_reaction_handler_reset_for_test();

    /* The handler wrote 6 SINGLE-SIDED rows (3 chosen-only, 3 rejected-only). */
    size_t rows = 0;
    HU_ASSERT_EQ(hu_dpo_pair_count(&col, &rows), HU_OK);
    HU_ASSERT_EQ(rows, 6u);

    /* Contrast — plain export drops all single-sided rows: the production bug. */
    hu_dpo_export_t plain = {0};
    HU_ASSERT_EQ(hu_dpo_export(&col, &alloc, &plain), HU_OK);
    HU_ASSERT_EQ(plain.count, 0u);
    hu_dpo_export_free(&alloc, &plain);

    /* Paired export zips same-prompt good/bad into 3 trainable two-sided pairs. */
    hu_dpo_export_t paired = {0};
    HU_ASSERT_EQ(hu_dpo_export_paired(&col, &alloc, &paired), HU_OK);
    HU_ASSERT_EQ(paired.count, 3u);
    for (size_t i = 0; i < paired.count; i++) {
        HU_ASSERT_TRUE(paired.pairs[i].chosen_len >= 4 && paired.pairs[i].rejected_len >= 4);
        HU_ASSERT_EQ(memcmp(paired.pairs[i].prompt, prompt, strlen(prompt)), 0);
        HU_ASSERT_EQ(memcmp(paired.pairs[i].chosen, good, strlen(good)), 0);
        HU_ASSERT_EQ(memcmp(paired.pairs[i].rejected, bad, strlen(bad)), 0);
    }

    /* Gold-standard hook: when HU_E2E_PAIRED_JSONL_OUT names a path, dump the
     * paired corpus (produced by the REAL hu_dpo_export_paired above) as
     * {prompt,chosen,rejected} JSONL so scripts/e2e-reaction-to-adapter-gold.sh
     * can feed it straight to `human ml dpo-train --backend mlx`. The fixed
     * strings above contain no JSON metacharacters, so no escaping is needed. */
    const char *jsonl_out = getenv("HU_E2E_PAIRED_JSONL_OUT");
    if (jsonl_out && jsonl_out[0]) {
        FILE *jf = fopen(jsonl_out, "wb");
        HU_ASSERT_NOT_NULL(jf);
        for (size_t i = 0; i < paired.count; i++) {
            fprintf(jf, "{\"prompt\": \"%.*s\", \"chosen\": \"%.*s\", \"rejected\": \"%.*s\"}\n",
                    (int)paired.pairs[i].prompt_len, paired.pairs[i].prompt,
                    (int)paired.pairs[i].chosen_len, paired.pairs[i].chosen,
                    (int)paired.pairs[i].rejected_len, paired.pairs[i].rejected);
        }
        fclose(jf);
    }

    /* The DPO trainer (HUML, in-process) consumes the reaction-derived pairs
     * end to end — the exact call lora_training_runner makes in production. */
    hu_rl_trainer_config_t tcfg = {
        .backend = HU_DPO_BACKEND_HUML,
        .beta = 0.1,
        .learning_rate = 0.1,
    };
    hu_rl_trainer_t trainer = {0};
    HU_ASSERT_EQ(hu_rl_trainer_create_dpo(&alloc, &tcfg, &trainer), HU_OK);
    hu_rl_trainer_metrics_t metrics = {0};
    HU_ASSERT_EQ(trainer.vtable->step(trainer.ctx, &alloc, paired.pairs, paired.count, &metrics),
                 HU_OK);
    HU_ASSERT_TRUE(metrics.iters_completed >= 1);

    trainer.vtable->deinit(trainer.ctx, &alloc);
    hu_dpo_export_free(&alloc, &paired);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);
#else
    HU_SKIP_IF(1, "SQLite + ML required for reaction→paired→train e2e");
#endif
}

void run_reaction_paired_train_e2e_tests(void) {
    HU_TEST_SUITE("reaction_paired_train_e2e");
    HU_RUN_TEST(reaction_stream_becomes_trainable_pairs_e2e);
}
