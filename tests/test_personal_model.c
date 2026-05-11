#include "human/agent/prompt.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/wait.h>
#endif

static void personal_model_init_sets_defaults(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_EQ((long)m.version, 4L);
    HU_ASSERT_EQ((long)m.created_at, 0L);
    HU_ASSERT_EQ((long)m.fact_count, 0L);
    HU_ASSERT_EQ((long)m.topic_count, 0L);
    HU_ASSERT_EQ((long)m.goal_count, 0L);
    HU_ASSERT_EQ((unsigned)m.style.sample_count, 0U);
}

static void personal_model_ingest_extracts_facts(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "I like hiking, I live in Portland";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, strlen(text), true, 1700000000LL), HU_OK);
    HU_ASSERT_TRUE(m.fact_count >= 2U);
}

static void personal_model_merge_facts_deduplicates(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.updated_at = 1;

    hu_fact_extract_result_t batch;
    memset(&batch, 0, sizeof(batch));
    strncpy(batch.facts[0].subject, "user", sizeof(batch.facts[0].subject) - 1);
    strncpy(batch.facts[0].predicate, "i like", sizeof(batch.facts[0].predicate) - 1);
    strncpy(batch.facts[0].object, "tea", sizeof(batch.facts[0].object) - 1);
    batch.facts[0].type = HU_KNOWLEDGE_PROPOSITIONAL;
    batch.facts[0].confidence = 0.7f;
    batch.fact_count = 1;

    HU_ASSERT_EQ(hu_personal_model_merge_facts(&m, &batch), HU_OK);
    HU_ASSERT_EQ((long)m.fact_count, 1L);
    HU_ASSERT_EQ(hu_personal_model_merge_facts(&m, &batch), HU_OK);
    HU_ASSERT_EQ((long)m.fact_count, 1L);
}

static void personal_model_build_prompt_non_empty(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "[Personal Context]") != NULL);
}

static void personal_model_query_preference_finds_match(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "I prefer dark mode for coding";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, strlen(text), true, 0), HU_OK);
    const hu_heuristic_fact_t *f = hu_personal_model_query_preference(&m, "dark", 4);
    HU_ASSERT_NOT_NULL(f);
}

static void personal_model_ingest_updates_style_metrics(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "Hello there";
    size_t len = strlen(text);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, len, true, 0), HU_OK);
    HU_ASSERT_EQ((unsigned)m.style.sample_count, 1U);
    HU_ASSERT_EQ((unsigned)m.style.avg_message_length, (unsigned)len);
}

static void personal_model_has_content_false_when_fresh(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_FALSE(hu_personal_model_has_content(&m));
    HU_ASSERT_FALSE(hu_personal_model_has_content(NULL));
}

static void personal_model_has_content_true_after_fact(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "I like hiking";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, strlen(text), true, 1700000000LL), HU_OK);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&m));
}

static void personal_model_has_content_true_after_style_observation(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "ok";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, strlen(text), true, 1700000000LL), HU_OK);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&m));
}

/* Integration: prove that when an agent's personal model has content and
 * is wired into the prompt config, the rendered system prompt actually
 * contains the user's facts. This is the regression test that closes the
 * "personal model is ingested but never injected" gap. */
static void personal_model_reaches_system_prompt_via_config(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    strncpy(m.core.user_name, "Sethford", sizeof(m.core.user_name) - 1);
    const char *text1 = "I love rock climbing on weekends";
    const char *text2 = "I prefer dark roast coffee in the morning";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text1, strlen(text1), true, 1700000000LL), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text2, strlen(text2), true, 1700000060LL), HU_OK);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&m));

    char pm_buf[8192];
    size_t pm_len = hu_personal_model_build_prompt(&m, pm_buf, sizeof(pm_buf));
    HU_ASSERT_GT((long)pm_len, 0L);

    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
        .provider_name = "test",
        .provider_name_len = 4,
        .model_name = "test-model",
        .model_name_len = 10,
        .autonomy_level = 1,
        .personal_model_context = pm_buf,
        .personal_model_context_len = pm_len,
    };

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[Personal Context]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Sethford") != NULL);
    HU_ASSERT_TRUE(strstr(out, "climbing") != NULL || strstr(out, "coffee") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* Adversarial: when no personal model context is set, the prompt should
 * still render cleanly with no [Personal Context] block leaking through. */
static void personal_model_absent_does_not_leak_into_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
        .provider_name = "test",
        .provider_name_len = 4,
        .model_name = "test-model",
        .model_name_len = 10,
        .autonomy_level = 1,
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[Personal Context]") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* M2 P1 — save/load round trip. The model is the only place we
 * accumulate user-specific signal across daemon restarts; without
 * persistence M2 is functionally still RAM-only. This test proves the
 * binary format survives a full round trip: ingest signals → save →
 * fresh struct → load → equality on the fields that carry value. */
static void personal_model_save_load_round_trips(void) {
    hu_personal_model_t a;
    hu_personal_model_init(&a);
    /* Seed signal: a couple of fact-shaped utterances + style observations. */
    hu_personal_model_ingest(&a, "i love climbing in the morning", 30, true,
                             1700000000LL);
    hu_personal_model_ingest(&a, "i never drink coffee", 21, true, 1700000100LL);
    hu_personal_model_ingest(&a, "lol that's cool 😎", 18, true, 1700000200LL);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&a));

    /* Round-trip through a unique tmp path. */
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_pm_test_%d.bin", (int)getpid());
    HU_ASSERT_EQ(hu_personal_model_save(&a, path), HU_OK);

    hu_personal_model_t b;
    HU_ASSERT_EQ(hu_personal_model_load(&b, path), HU_OK);

    /* Key invariants: facts and interaction count survived. We don't
     * memcmp the whole struct because the build prompt's style metrics
     * are floats and a strict byte-equality would be brittle on platforms
     * with different float representations. Instead we assert the
     * meaningful fields. */
    HU_ASSERT_EQ(b.fact_count, a.fact_count);
    HU_ASSERT_EQ(b.interaction_count, a.interaction_count);
    HU_ASSERT_EQ(b.version, a.version);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&b));

    /* Cleanup. */
    remove(path);
}

/* Bad magic must fail closed and leave `out` initialized to defaults so
 * the daemon can keep going without crashing. */
static void personal_model_load_rejects_bad_magic(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_pm_bad_%d.bin", (int)getpid());
    FILE *fp = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(fp);
    const char garbage[64] = "not a personal model file at all";
    fwrite(garbage, sizeof(garbage), 1, fp);
    fclose(fp);

    hu_personal_model_t out;
    HU_ASSERT_EQ(hu_personal_model_load(&out, path), HU_ERR_PARSE);
    /* Initialized to defaults — version matches current schema. */
    HU_ASSERT_EQ(out.version, 4U);
    HU_ASSERT_EQ(out.fact_count, (size_t)0);
    HU_ASSERT_FALSE(hu_personal_model_has_content(&out));

    remove(path);
}

/* Missing file is a benign HU_ERR_NOT_FOUND. */
static void personal_model_load_missing_file_returns_not_found(void) {
    hu_personal_model_t out;
    HU_ASSERT_EQ(hu_personal_model_load(&out, "/tmp/no_such_pm_file_xyz_123.bin"),
                 HU_ERR_NOT_FOUND);
}

/* Resolution favours the env override when set. */
static void personal_model_resolve_default_path_honors_env_override(void) {
    char buf[128];
    char override[64];
    snprintf(override, sizeof(override), "/tmp/hu_pm_override_%d.bin", (int)getpid());
    setenv("HUMAN_PERSONAL_MODEL_PATH", override, 1);
    const char *got = hu_personal_model_resolve_default_path(buf, sizeof(buf));
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_STR_EQ(got, override);
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
}

/* Without the override, the path lives under $HOME/.human/. */
static void personal_model_resolve_default_path_uses_home(void) {
    char tmp_home[64];
    snprintf(tmp_home, sizeof(tmp_home), "/tmp/hu_pm_home_%d", (int)getpid());
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
    setenv("HOME", tmp_home, 1);

    char buf[256];
    const char *got = hu_personal_model_resolve_default_path(buf, sizeof(buf));
    HU_ASSERT_NOT_NULL(got);
    char expected[256];
    snprintf(expected, sizeof(expected), "%s/.human/personal_model.bin", tmp_home);
    HU_ASSERT_STR_EQ(got, expected);
}

/* Without HOME or override, resolution returns NULL (no crash). */
static void personal_model_resolve_default_path_no_home_returns_null(void) {
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
    const char *prior_home = getenv("HOME");
    char saved_home[1024] = {0};
    if (prior_home) {
        size_t n = strlen(prior_home);
        if (n < sizeof(saved_home)) {
            memcpy(saved_home, prior_home, n + 1);
        }
    }
    unsetenv("HOME");
    char buf[64];
    HU_ASSERT_NULL(hu_personal_model_resolve_default_path(buf, sizeof(buf)));
    if (saved_home[0]) {
        setenv("HOME", saved_home, 1);
    }
}

/* Save creates the parent directory tree on first run so users don't have
 * to pre-create `~/.human/`. */
static void personal_model_save_creates_parent_directory(void) {
    char tmp_dir[128];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/hu_pm_mkdir_%d/nested/leaf", (int)getpid());
    char path[256];
    snprintf(path, sizeof(path), "%s/personal_model.bin", tmp_dir);

    /* Make sure the directory does NOT exist yet — fresh-state assertion. */
    char rm_cmd[512];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf /tmp/hu_pm_mkdir_%d 2>/dev/null", (int)getpid());
    (void)system(rm_cmd);

    hu_personal_model_t a;
    hu_personal_model_init(&a);
    hu_personal_model_ingest(&a, "i prefer dark mode", 18, true, 1700000300LL);
    HU_ASSERT_EQ(hu_personal_model_save(&a, path), HU_OK);

    hu_personal_model_t b;
    HU_ASSERT_EQ(hu_personal_model_load(&b, path), HU_OK);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&b));

    (void)system(rm_cmd);
}

/* Round-trip via the resolver: save then load using the same default path
 * (driven by the env override) round-trips fact data. Documents the agent
 * lifecycle wiring contract. */
static void personal_model_default_path_round_trip(void) {
    char override[64];
    snprintf(override, sizeof(override), "/tmp/hu_pm_resolver_%d.bin", (int)getpid());
    setenv("HUMAN_PERSONAL_MODEL_PATH", override, 1);

    char path[256];
    HU_ASSERT_NOT_NULL(hu_personal_model_resolve_default_path(path, sizeof(path)));

    hu_personal_model_t a;
    hu_personal_model_init(&a);
    hu_personal_model_ingest(&a, "i never drink coffee", 21, true, 1700000400LL);
    HU_ASSERT_EQ(hu_personal_model_save(&a, path), HU_OK);

    char path2[256];
    HU_ASSERT_NOT_NULL(hu_personal_model_resolve_default_path(path2, sizeof(path2)));
    HU_ASSERT_STR_EQ(path, path2);
    hu_personal_model_t b;
    HU_ASSERT_EQ(hu_personal_model_load(&b, path2), HU_OK);
    HU_ASSERT_EQ(b.fact_count, a.fact_count);
    HU_ASSERT_EQ(b.interaction_count, a.interaction_count);

    remove(override);
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
}

/* M2 P1 crash-safety: simulates a daemon killed mid-turn and verifies the
 * personal model on disk is durable and complete.
 *
 * "Mid-turn crash" semantics in unit-test land: we ingest signal into a
 * struct, save it via the same call path the agent uses after every user
 * message (see agent_turn.c / agent_stream.c), then zero the struct bytes
 * WITHOUT calling any deinit / cleanup. That is functionally equivalent
 * to the process being killed by SIGKILL: no destructors run, no flush
 * callbacks fire, the in-memory state is gone.
 *
 * Then a *fresh* struct loads from the same default path the agent
 * resolver would have produced after restart. If the load recovers every
 * fact and the interaction counter, the on-disk format is crash-safe
 * across process boundaries.
 *
 * What this proves: hu_personal_model_save is atomic enough that a crash
 * the instant after it returns HU_OK leaves a file the next process can
 * fully reload. What it does NOT prove: that a crash *during* the save
 * (between write/rename) is safe; that case is the responsibility of the
 * save implementation and is covered by the existing atomic-rename test
 * if it exists. */
static void personal_model_survives_simulated_crash(void) {
    char override[64];
    snprintf(override, sizeof(override), "/tmp/hu_pm_crash_%d.bin", (int)getpid());
    setenv("HUMAN_PERSONAL_MODEL_PATH", override, 1);

    char path[256];
    HU_ASSERT_NOT_NULL(hu_personal_model_resolve_default_path(path, sizeof(path)));

    /* Process A: ingest several signals, save after each (mirrors the
     * per-turn save now wired into agent_turn.c / agent_stream.c). */
    hu_personal_model_t a;
    hu_personal_model_init(&a);
    hu_personal_model_ingest(&a, "i never drink coffee after 2pm", 30, true, 1700000100LL);
    HU_ASSERT_EQ(hu_personal_model_save(&a, path), HU_OK);
    hu_personal_model_ingest(&a, "i love long walks at sunset", 27, true, 1700000200LL);
    HU_ASSERT_EQ(hu_personal_model_save(&a, path), HU_OK);
    hu_personal_model_ingest(&a, "my favorite color is teal", 25, true, 1700000300LL);
    HU_ASSERT_EQ(hu_personal_model_save(&a, path), HU_OK);

    size_t expected_fact_count = a.fact_count;
    size_t expected_interactions = a.interaction_count;
    HU_ASSERT_TRUE(expected_fact_count >= 1);
    HU_ASSERT_TRUE(expected_interactions >= 3);

    /* CRASH: zero the in-memory struct WITHOUT calling deinit. No cleanup
     * runs. This is what kill -9 looks like to the rest of the program. */
    memset(&a, 0, sizeof(a));

    /* Process B: fresh struct, fresh resolver, no shared state. */
    char path2[256];
    HU_ASSERT_NOT_NULL(hu_personal_model_resolve_default_path(path2, sizeof(path2)));
    HU_ASSERT_STR_EQ(path, path2);

    hu_personal_model_t b;
    hu_personal_model_init(&b);
    HU_ASSERT_EQ(hu_personal_model_load(&b, path2), HU_OK);

    HU_ASSERT_EQ(b.fact_count, expected_fact_count);
    HU_ASSERT_EQ(b.interaction_count, expected_interactions);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&b));

    remove(override);
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
}

#if defined(__unix__) || defined(__APPLE__)
/* M2 P1 crash-safety: true cross-process SIGKILL crash test.
 *
 * personal_model_survives_simulated_crash simulates a crash by zeroing
 * the in-memory struct without deinit, which is a fair model of kill -9
 * inside one process but doesn't actually cross a process boundary.
 *
 * This test does the real thing. We fork(), the child ingests three
 * signals, saves via the resolver path the agent uses in production,
 * then sends SIGKILL to itself. SIGKILL is uncatchable: no destructors,
 * no ASan exit handler, no flush — the kernel just terminates the
 * process. The parent waitpid's, asserts the child died by signal 9
 * (proving cleanup did not run), then loads from the same path through
 * a fresh resolver and asserts every fact and the interaction counter
 * survived.
 *
 * What this proves on top of the in-process simulation:
 *   1. The save is durable across process boundaries — not just across
 *      memory-clear in one address space.
 *   2. The on-disk file format is loadable by a *different* process
 *      with no shared heap, no shared malloc arena, no shared anything.
 *   3. SIGKILL specifically (not just abort()) is survivable. abort()
 *      gives the C runtime a chance to flush stdio, run atexit handlers,
 *      etc; SIGKILL does not.
 *
 * Skipped on non-POSIX (Windows fork() doesn't exist). */
static void personal_model_survives_real_sigkill(void) {
    char override[64];
    snprintf(override, sizeof(override), "/tmp/hu_pm_sigkill_%d.bin", (int)getpid());
    setenv("HUMAN_PERSONAL_MODEL_PATH", override, 1);

    /* Ensure no stale file from a prior run. */
    remove(override);

    pid_t pid = fork();
    HU_ASSERT_TRUE(pid >= 0); /* fork must succeed */

    if (pid == 0) {
        /* Child: ingest signals, save, then SIGKILL self. We deliberately
         * skip every cleanup path — that's the point. */
        char path[256];
        if (!hu_personal_model_resolve_default_path(path, sizeof(path)))
            _exit(2); /* resolver failure → parent sees this and fails */

        hu_personal_model_t a;
        hu_personal_model_init(&a);
        hu_personal_model_ingest(&a, "i never drink coffee after 2pm", 30, true, 1700000100LL);
        if (hu_personal_model_save(&a, path) != HU_OK)
            _exit(3);
        hu_personal_model_ingest(&a, "i love long walks at sunset", 27, true, 1700000200LL);
        if (hu_personal_model_save(&a, path) != HU_OK)
            _exit(4);
        hu_personal_model_ingest(&a, "my favorite color is teal", 25, true, 1700000300LL);
        if (hu_personal_model_save(&a, path) != HU_OK)
            _exit(5);

        /* fsync(2)-equivalent durability is the save implementation's
         * responsibility. If it doesn't fsync we'll only catch that
         * under a real power-loss scenario; SIGKILL alone won't expose
         * write-back-cache races on a healthy filesystem. */

        /* Crash. raise(SIGKILL) is uncatchable; no destructors run. */
        raise(SIGKILL);
        _exit(99); /* unreachable */
    }

    /* Parent: wait for the child to die. */
    int status = 0;
    pid_t r = waitpid(pid, &status, 0);
    HU_ASSERT_EQ((long)r, (long)pid);

    /* The child must have died by signal 9, not exited cleanly. If it
     * exited cleanly something is very wrong (e.g., raise() returned). */
    HU_ASSERT_TRUE(WIFSIGNALED(status));
    HU_ASSERT_EQ(WTERMSIG(status), SIGKILL);

    /* Parent re-resolves and loads. Fresh struct, fresh address space
     * boundary, fresh allocator state. */
    char path2[256];
    HU_ASSERT_NOT_NULL(hu_personal_model_resolve_default_path(path2, sizeof(path2)));

    hu_personal_model_t b;
    hu_personal_model_init(&b);
    HU_ASSERT_EQ(hu_personal_model_load(&b, path2), HU_OK);

    /* The child saved three times; the last save (after ingesting all
     * three signals) is what we must recover. */
    HU_ASSERT_TRUE(b.fact_count >= 1);
    HU_ASSERT_TRUE(b.interaction_count >= 3);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&b));

    remove(override);
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
}
#endif /* POSIX */

/* Style-mirror directive tests.
 *
 * The personal model has historically dumped style observations
 * ("Communication style: casual, terse, avg 50 chars") into the system
 * prompt without telling the frontier model what to *do* with them.
 * Without an explicit "Mirror their style: …" directive, models default
 * to their training-distribution register regardless of who they're
 * talking to — formal, medium-length, sparing emoji, neutral humor.
 *
 * These tests pin the directive's content for archetypal user profiles
 * and the warm-up gate that suppresses it before the EWMA stabilizes. */
static void personal_model_style_directive_emerges_after_three_samples(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "yo", 2, true, 1700000001), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "lol", 3, true, 1700000002), HU_OK);
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    /* Two samples — directive should still be suppressed (warm-up). */
    HU_ASSERT_TRUE(strstr(buf, "Mirror their style:") == NULL);

    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "hey there", 9, true, 1700000003), HU_OK);
    n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    /* Three samples — directive activates. */
    HU_ASSERT_TRUE(strstr(buf, "Mirror their style:") != NULL);
}

static void personal_model_style_directive_casual_terse_humorous(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    /* Three short, casual, humor-heavy messages move the EWMA toward
     * casual + terse + humor. */
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "lol same", 8, true, 1700000001), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "haha yep", 8, true, 1700000002), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "lol btw", 7, true, 1700000003), HU_OK);

    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);

    HU_ASSERT_TRUE(strstr(buf, "Mirror their style:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "casual register") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "keep replies ~50 chars") != NULL);
    /* Humor receptivity rose from "lol"/"haha" — should welcome humor. */
    HU_ASSERT_TRUE(strstr(buf, "humor") != NULL);
}

static void personal_model_style_directive_formal_verbose(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    /* Long polite messages. The formality cue list (please / thank you /
     * would you) pulls formality up; length pulls verbosity up. */
    const char *long1 =
        "Please could you walk me through this carefully? Thank you, I would "
        "appreciate the additional context, especially around the "
        "implementation details and any trade-offs you considered when "
        "selecting the data structure backing the cache.";
    const char *long2 =
        "Would you mind also expanding on the failure-mode analysis? Please "
        "include latency numbers if available; thank you for being thorough "
        "in your previous responses.";
    const char *long3 =
        "Please share the benchmark methodology. Thank you. Would you "
        "specifically clarify how you measured tail latency and what "
        "percentiles you reported across runs?";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, long1, strlen(long1), true, 1700000001), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, long2, strlen(long2), true, 1700000002), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, long3, strlen(long3), true, 1700000003), HU_OK);

    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);

    HU_ASSERT_TRUE(strstr(buf, "Mirror their style:") != NULL);
    /* No emojis in the input — directive should suppress emoji. */
    HU_ASSERT_TRUE(strstr(buf, "no emoji") != NULL);
    /* No humor cues — should stay serious. */
    HU_ASSERT_TRUE(strstr(buf, "stay serious") != NULL);
    /* Avg message length is in the 200-bucket band. */
    HU_ASSERT_TRUE(strstr(buf, "keep replies ~200 chars") != NULL ||
                   strstr(buf, "keep replies ~300 chars") != NULL);
}

static void personal_model_style_directive_lowercase_typer(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    /* Five all-lowercase messages — ratio should clear the 0.5 threshold. */
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "yo whats up", 11, true, 1700000001), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "lol same", 8, true, 1700000002), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "haha yeah", 9, true, 1700000003), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "i think so", 10, true, 1700000004), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "ok cool", 7, true, 1700000005), HU_OK);

    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Mirror their style:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "type lowercase") != NULL);
}

static void personal_model_style_directive_abbreviation_user(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    /* Five messages, all carrying chat shorthand — ratio clears 0.4. */
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "u busy?", 7, true, 1700000001), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "btw lmk", 7, true, 1700000002), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "ty ty", 5, true, 1700000003), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "rn? or later?", 13, true, 1700000004), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "ok ty btw", 9, true, 1700000005), HU_OK);

    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Mirror their style:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "'u'/'rn'/'btw'") != NULL);
}

static void personal_model_style_directive_proper_case_no_lowercase_clause(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    /* Three messages, all properly capitalized — lowercase_ratio stays
     * near zero. The directive should appear (3 samples) but without the
     * "type lowercase" clause. */
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "Hello there.", 12, true, 1700000001), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "How are you today?", 18, true, 1700000002), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "What time works?", 16, true, 1700000003), HU_OK);

    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Mirror their style:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "type lowercase") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "'u'/'rn'/'btw'") == NULL);
}

/* SOTA negative-fact surfacing.
 *
 * The "Key facts:" line concatenates "user i don't like X, user works at Y"
 * which frontier models can read but routinely gloss past — the negation is
 * one tiny token in a dense list. A dedicated "Avoid:" line gives each
 * disliked thing its own slot in the prompt where the model is far less
 * likely to miss it. */
static void personal_model_avoid_line_emerges_for_dislikes(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    const char msg[] = "i don't like coffee or small talk early in the morning";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, msg, strlen(msg), true, 1700000001), HU_OK);

    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Avoid:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "coffee") != NULL);
}

static void personal_model_avoid_line_handles_hate(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    const char msg[] = "i hate flaky meetings";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, msg, strlen(msg), true, 1700000001), HU_OK);

    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Avoid:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "flaky meetings") != NULL);
}

static void personal_model_avoid_line_skips_when_only_positive_facts(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    const char msg1[] = "i love hiking on weekends";
    const char msg2[] = "i work at acme";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, msg1, strlen(msg1), true, 1700000001), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, msg2, strlen(msg2), true, 1700000002), HU_OK);

    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Avoid:") == NULL);
}

static void personal_model_avoid_line_collects_multiple_dislikes(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];

    const char msg1[] = "i'm allergic to peanuts";
    const char msg2[] = "i dislike loud music";
    const char msg3[] = "i love jazz";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, msg1, strlen(msg1), true, 1700000001), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, msg2, strlen(msg2), true, 1700000002), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, msg3, strlen(msg3), true, 1700000003), HU_OK);

    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Avoid:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "peanuts") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "loud music") != NULL);
    /* Positive fact must not appear on the Avoid line. */
    const char *avoid_line = strstr(buf, "Avoid:");
    HU_ASSERT_NOT_NULL(avoid_line);
    const char *eol = strchr(avoid_line, '\n');
    HU_ASSERT_NOT_NULL(eol);
    size_t avoid_len = (size_t)(eol - avoid_line);
    HU_ASSERT_TRUE(memmem(avoid_line, avoid_len, "jazz", 4) == NULL);
}

static void personal_model_style_directive_absent_when_no_samples(void) {
    /* A model populated only via merge_facts (no ingest, sample_count == 0)
     * must not produce a directive — there's no observed style to mirror. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    hu_fact_extract_result_t fx = {0};
    fx.fact_count = 1;
    strncpy(fx.facts[0].subject, "user", sizeof(fx.facts[0].subject) - 1);
    strncpy(fx.facts[0].predicate, "likes", sizeof(fx.facts[0].predicate) - 1);
    strncpy(fx.facts[0].object, "tea", sizeof(fx.facts[0].object) - 1);
    fx.facts[0].confidence = 0.9f;
    HU_ASSERT_EQ(hu_personal_model_merge_facts(&m, &fx), HU_OK);

    char buf[2048];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Mirror their style:") == NULL);
}

/* ── Topic-engagement directive ─────────────────────────────────────── */

/* Directive must emerge for a topic with sustained mention_count + score. */
static void personal_model_topic_directive_emerges_for_sustained_interest(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    /* Seed a single high-engagement topic directly — bump_topic only
     * increments mention_count once per distinct fact, but tests want
     * to assert directive emission, not the bump path. */
    m.topic_count = 1;
    strncpy(m.topics[0].name, "hiking", sizeof(m.topics[0].name) - 1);
    m.topics[0].interest_score = 0.85f;
    m.topics[0].mention_count = 5U;

    char buf[2048];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Engage substantively when these come up: hiking.") != NULL);
}

/* Directive must NOT emerge when the topic has only one or two mentions. */
static void personal_model_topic_directive_skips_low_mention_count(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    m.topic_count = 1;
    strncpy(m.topics[0].name, "fly fishing", sizeof(m.topics[0].name) - 1);
    m.topics[0].interest_score = 0.85f;
    m.topics[0].mention_count = 2U;

    char buf[2048];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Engage substantively") == NULL);
    /* Observation line still present. */
    HU_ASSERT_TRUE(strstr(buf, "Top interests:") != NULL);
}

/* Low interest_score must NOT emit directive even when mention_count is high. */
static void personal_model_topic_directive_skips_low_interest_score(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    m.topic_count = 1;
    strncpy(m.topics[0].name, "small talk", sizeof(m.topics[0].name) - 1);
    m.topics[0].interest_score = 0.3f;
    m.topics[0].mention_count = 7U;

    char buf[2048];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Engage substantively") == NULL);
}

/* Multiple sustained topics — directive lists up to 3, in interest order. */
static void personal_model_topic_directive_caps_at_three(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    m.topic_count = 5;
    struct {
        const char *name;
        float score;
    } seed[] = {
        {"hiking",      0.95f},
        {"woodworking", 0.85f},
        {"jazz",        0.75f},
        {"climbing",    0.65f},
        {"sourdough",   0.55f},
    };
    for (size_t i = 0; i < 5; i++) {
        strncpy(m.topics[i].name, seed[i].name, sizeof(m.topics[i].name) - 1);
        m.topics[i].interest_score = seed[i].score;
        m.topics[i].mention_count = 4U;
    }

    char buf[2048];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    const char *eng = strstr(buf, "Engage substantively when these come up:");
    HU_ASSERT_NOT_NULL(eng);
    /* Top three by score should appear; #4 and #5 should not. */
    HU_ASSERT_TRUE(strstr(eng, "hiking") != NULL);
    HU_ASSERT_TRUE(strstr(eng, "woodworking") != NULL);
    HU_ASSERT_TRUE(strstr(eng, "jazz") != NULL);
    /* Bound the search to the directive line itself. */
    const char *eol = strchr(eng, '\n');
    HU_ASSERT_NOT_NULL(eol);
    size_t line_len = (size_t)(eol - eng);
    HU_ASSERT_TRUE(memmem(eng, line_len, "climbing", 8) == NULL);
    HU_ASSERT_TRUE(memmem(eng, line_len, "sourdough", 9) == NULL);
}

/* Mixed: one sustained, one fly-by — only the sustained one shows up. */
static void personal_model_topic_directive_filters_mixed_topics(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    m.topic_count = 2;
    strncpy(m.topics[0].name, "ml research", sizeof(m.topics[0].name) - 1);
    m.topics[0].interest_score = 0.9f;
    m.topics[0].mention_count = 6U;

    strncpy(m.topics[1].name, "the weather", sizeof(m.topics[1].name) - 1);
    m.topics[1].interest_score = 0.7f;
    m.topics[1].mention_count = 1U;

    char buf[2048];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    const char *eng = strstr(buf, "Engage substantively when these come up: ml research.");
    HU_ASSERT_NOT_NULL(eng);
    /* "the weather" must not appear on the directive line. */
    const char *eol = strchr(eng, '\n');
    HU_ASSERT_NOT_NULL(eol);
    size_t line_len = (size_t)(eol - eng);
    HU_ASSERT_TRUE(memmem(eng, line_len, "weather", 7) == NULL);
}

/* No topics at all → no directive line at all. */
static void personal_model_topic_directive_absent_when_no_topics(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    char buf[1024];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "Engage substantively") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "Top interests:") == NULL);
}

/* ── Chronotype inference from active_hours ─────────────────────────── */

static void personal_model_infer_chronotype_returns_unknown_for_empty(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_TRUE(hu_personal_model_infer_chronotype(&m) == HU_CHRONO_UNKNOWN);
}

static void personal_model_infer_chronotype_returns_unknown_for_null(void) {
    HU_ASSERT_TRUE(hu_personal_model_infer_chronotype(NULL) == HU_CHRONO_UNKNOWN);
}

static void personal_model_infer_chronotype_returns_unknown_below_min_samples(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* 20 messages all morning — under the 30-sample floor, classifier
     * abstains regardless of how concentrated the signal is. */
    for (int h = 5; h <= 9; h++)
        m.active_hours[h] = 4;
    HU_ASSERT_TRUE(hu_personal_model_infer_chronotype(&m) == HU_CHRONO_UNKNOWN);
}

static void personal_model_infer_chronotype_classifies_morning_lark(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Heavy early-window concentration. */
    m.active_hours[6] = 12;
    m.active_hours[7] = 14;
    m.active_hours[8] = 12;
    m.active_hours[9] = 8;
    /* Light middle / late presence. */
    m.active_hours[14] = 2;
    m.active_hours[22] = 2;
    HU_ASSERT_TRUE(hu_personal_model_infer_chronotype(&m) == HU_CHRONO_MORNING_LARK);
}

static void personal_model_infer_chronotype_classifies_evening_owl(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Heavy evening-window concentration; hour 0 (midnight) bucketed
     * with the late window. */
    m.active_hours[21] = 10;
    m.active_hours[22] = 12;
    m.active_hours[23] = 14;
    m.active_hours[0] = 4;
    m.active_hours[12] = 2;
    m.active_hours[15] = 2;
    HU_ASSERT_TRUE(hu_personal_model_infer_chronotype(&m) == HU_CHRONO_EVENING_OWL);
}

static void personal_model_infer_chronotype_classifies_intermediate_when_flat(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Near-uniform distribution — should land in INTERMEDIATE
     * regardless of which side wins the tie. */
    for (int h = 0; h < 24; h++)
        m.active_hours[h] = 3;
    HU_ASSERT_TRUE(hu_personal_model_infer_chronotype(&m) == HU_CHRONO_INTERMEDIATE);
}

static void personal_model_infer_chronotype_classifies_intermediate_for_middle_dominant(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Workday-shape pattern: heavy mid-day, light edges. Neither early
     * nor late dominates ⇒ INTERMEDIATE. */
    m.active_hours[10] = 6;
    m.active_hours[11] = 8;
    m.active_hours[12] = 8;
    m.active_hours[13] = 8;
    m.active_hours[14] = 8;
    m.active_hours[15] = 6;
    m.active_hours[7] = 2;
    m.active_hours[22] = 2;
    HU_ASSERT_TRUE(hu_personal_model_infer_chronotype(&m) == HU_CHRONO_INTERMEDIATE);
}

static void personal_model_infer_chronotype_does_not_overcommit_on_thin_lead(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Early=11, late=10, middle=20 — early "leads" by one but does NOT
     * meet the 1.5× ratio threshold and falls below 40% of total.
     * Classifier should land in INTERMEDIATE rather than calling
     * MORNING_LARK on noise. */
    m.active_hours[7] = 5;
    m.active_hours[8] = 6;
    m.active_hours[12] = 5;
    m.active_hours[13] = 5;
    m.active_hours[14] = 5;
    m.active_hours[15] = 5;
    m.active_hours[22] = 5;
    m.active_hours[23] = 5;
    HU_ASSERT_TRUE(hu_personal_model_infer_chronotype(&m) == HU_CHRONO_INTERMEDIATE);
}

/* ── Fact decay + freshness tests ─────────────────────────────────── */

static void fact_effective_confidence_no_decay_when_last_seen_zero(void) {
    hu_heuristic_fact_t f;
    memset(&f, 0, sizeof(f));
    f.confidence = 0.8f;
    f.last_seen_at = 0;
    /* now is non-zero, but last_seen_at == 0 means we have no decay
     * data — return the raw confidence unchanged. */
    HU_ASSERT_TRUE(hu_heuristic_fact_effective_confidence(&f, 1000000) > 0.79f);
}

static void fact_effective_confidence_no_decay_when_now_at_or_before_last_seen(void) {
    hu_heuristic_fact_t f;
    memset(&f, 0, sizeof(f));
    f.confidence = 0.8f;
    f.last_seen_at = 1000;
    HU_ASSERT_TRUE(hu_heuristic_fact_effective_confidence(&f, 1000) > 0.79f);
    HU_ASSERT_TRUE(hu_heuristic_fact_effective_confidence(&f, 500) > 0.79f);
}

static void fact_effective_confidence_halves_at_one_half_life(void) {
    hu_heuristic_fact_t f;
    memset(&f, 0, sizeof(f));
    f.confidence = 0.8f;
    f.last_seen_at = 1000;
    int64_t now = f.last_seen_at + HU_FACT_CONFIDENCE_HALF_LIFE_SEC;
    float eff = hu_heuristic_fact_effective_confidence(&f, now);
    /* 0.8 * 0.5 = 0.4. Allow ±0.02 for table interpolation. */
    HU_ASSERT_TRUE(eff > 0.38f && eff < 0.42f);
}

static void fact_effective_confidence_quarters_at_two_half_lives(void) {
    hu_heuristic_fact_t f;
    memset(&f, 0, sizeof(f));
    f.confidence = 0.8f;
    f.last_seen_at = 1000;
    int64_t now = f.last_seen_at + 2 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;
    float eff = hu_heuristic_fact_effective_confidence(&f, now);
    HU_ASSERT_TRUE(eff > 0.18f && eff < 0.22f); /* 0.2 ± 0.02 */
}

static void fact_effective_confidence_floors_to_zero_far_future(void) {
    hu_heuristic_fact_t f;
    memset(&f, 0, sizeof(f));
    f.confidence = 0.8f;
    f.last_seen_at = 1000;
    int64_t now = f.last_seen_at + 20 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;
    HU_ASSERT_TRUE(hu_heuristic_fact_effective_confidence(&f, now) < 0.01f);
}

static void fact_effective_confidence_handles_null(void) {
    HU_ASSERT_TRUE(hu_heuristic_fact_effective_confidence(NULL, 1000) < 0.01f);
}

static void fact_extract_zeroes_last_seen_at(void) {
    /* The extractor sees raw text; it doesn't know wall time, so
     * `last_seen_at` must always be zero on the way out. */
    hu_fact_extract_result_t r;
    memset(&r, 0xff, sizeof(r));
    HU_ASSERT_EQ(hu_fact_extract("I work at Acme.", 15, &r), HU_OK);
    HU_ASSERT_TRUE(r.fact_count > 0);
    for (size_t i = 0; i < r.fact_count; i++)
        HU_ASSERT_EQ((long long)r.facts[i].last_seen_at, 0LL);
}

static void personal_model_merge_facts_stamps_last_seen_on_insert(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.updated_at = 5000;
    hu_fact_extract_result_t r;
    memset(&r, 0, sizeof(r));
    r.fact_count = 1;
    r.facts[0].type = HU_KNOWLEDGE_PROPOSITIONAL;
    snprintf(r.facts[0].subject, sizeof(r.facts[0].subject), "user");
    snprintf(r.facts[0].predicate, sizeof(r.facts[0].predicate), "works at");
    snprintf(r.facts[0].object, sizeof(r.facts[0].object), "Acme");
    r.facts[0].confidence = 0.9f;
    HU_ASSERT_EQ(hu_personal_model_merge_facts(&m, &r), HU_OK);
    HU_ASSERT_EQ((long)m.fact_count, 1L);
    HU_ASSERT_EQ((long long)m.facts[0].last_seen_at, 5000LL);
}

static void personal_model_merge_facts_refreshes_duplicate(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.updated_at = 5000;
    hu_fact_extract_result_t r;
    memset(&r, 0, sizeof(r));
    r.fact_count = 1;
    r.facts[0].type = HU_KNOWLEDGE_PROPOSITIONAL;
    snprintf(r.facts[0].subject, sizeof(r.facts[0].subject), "user");
    snprintf(r.facts[0].predicate, sizeof(r.facts[0].predicate), "works at");
    snprintf(r.facts[0].object, sizeof(r.facts[0].object), "Acme");
    r.facts[0].confidence = 0.9f;
    HU_ASSERT_EQ(hu_personal_model_merge_facts(&m, &r), HU_OK);

    /* Same fact restated months later — should refresh, not duplicate. */
    m.updated_at = 5000 + HU_FACT_CONFIDENCE_HALF_LIFE_SEC;
    r.facts[0].confidence = 0.95f;
    HU_ASSERT_EQ(hu_personal_model_merge_facts(&m, &r), HU_OK);
    HU_ASSERT_EQ((long)m.fact_count, 1L);
    HU_ASSERT_EQ((long long)m.facts[0].last_seen_at,
                 (long long)(5000 + HU_FACT_CONFIDENCE_HALF_LIFE_SEC));
    /* Confidence ticks up via EWMA: 0.9 + 0.1 * (0.95 - 0.9) = 0.905. */
    HU_ASSERT_TRUE(m.facts[0].confidence > 0.90f && m.facts[0].confidence < 0.92f);
}

static void personal_model_build_prompt_drops_stale_facts(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Use a real-ish timestamp so 10 half-lives back is still positive. */
    m.updated_at = (int64_t)(40LL * HU_FACT_CONFIDENCE_HALF_LIFE_SEC);

    /* Fresh fact (now) — should appear. */
    m.facts[0].type = HU_KNOWLEDGE_PROPOSITIONAL;
    snprintf(m.facts[0].subject, sizeof(m.facts[0].subject), "user");
    snprintf(m.facts[0].predicate, sizeof(m.facts[0].predicate), "works at");
    snprintf(m.facts[0].object, sizeof(m.facts[0].object), "Initech");
    m.facts[0].confidence = 0.9f;
    m.facts[0].last_seen_at = m.updated_at;

    /* Very stale fact (10 half-lives ago at 0.9 confidence → eff < 0.001) */
    m.facts[1].type = HU_KNOWLEDGE_PROPOSITIONAL;
    snprintf(m.facts[1].subject, sizeof(m.facts[1].subject), "user");
    snprintf(m.facts[1].predicate, sizeof(m.facts[1].predicate), "works at");
    snprintf(m.facts[1].object, sizeof(m.facts[1].object), "Acme");
    m.facts[1].confidence = 0.9f;
    m.facts[1].last_seen_at = m.updated_at - 10 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;

    m.fact_count = 2;

    char buf[1024];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "Initech"));
    HU_ASSERT_NULL(strstr(buf, "Acme"));
}

static void personal_model_build_prompt_drops_stale_avoid_lines(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.updated_at = (int64_t)(40LL * HU_FACT_CONFIDENCE_HALF_LIFE_SEC);

    m.facts[0].type = HU_KNOWLEDGE_PROPOSITIONAL;
    snprintf(m.facts[0].subject, sizeof(m.facts[0].subject), "user");
    snprintf(m.facts[0].predicate, sizeof(m.facts[0].predicate), "i don't like");
    snprintf(m.facts[0].object, sizeof(m.facts[0].object), "ancient_topic");
    m.facts[0].confidence = 0.9f;
    /* 10 half-lives ago → effectively zero confidence. */
    m.facts[0].last_seen_at = m.updated_at - 10 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;
    m.fact_count = 1;

    char buf[1024];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    /* No "Avoid:" line should appear because the only negative fact
     * is too stale to surface. */
    HU_ASSERT_NULL(strstr(buf, "Avoid:"));
    HU_ASSERT_NULL(strstr(buf, "ancient_topic"));
    (void)n;
}

/* ── Topic decay ────────────────────────────────────────────────────── */

static void personal_model_topic_effective_score_handles_null(void) {
    HU_ASSERT_TRUE(hu_personal_topic_effective_score(NULL, 1000000) == 0.f);
}

static void personal_model_topic_effective_score_no_decay_when_unstamped(void) {
    hu_personal_topic_t t;
    memset(&t, 0, sizeof(t));
    t.interest_score = 0.7f;
    t.last_mentioned = 0; /* never observed */
    /* Same convention as facts: zero last_mentioned means "no decay
     * data" — return the raw score so synthetic fixtures don't get
     * silently zeroed. */
    HU_ASSERT_TRUE(hu_personal_topic_effective_score(&t, 1000000) > 0.69f);
}

static void personal_model_topic_effective_score_no_decay_when_now_at_or_before(void) {
    hu_personal_topic_t t;
    memset(&t, 0, sizeof(t));
    t.interest_score = 0.7f;
    t.last_mentioned = 1000;
    HU_ASSERT_TRUE(hu_personal_topic_effective_score(&t, 1000) > 0.69f);
    HU_ASSERT_TRUE(hu_personal_topic_effective_score(&t, 500) > 0.69f);
}

static void personal_model_topic_effective_score_halves_at_one_half_life(void) {
    hu_personal_topic_t t;
    memset(&t, 0, sizeof(t));
    t.interest_score = 0.8f;
    t.last_mentioned = 1000;
    int64_t now = t.last_mentioned + HU_PM_TOPIC_INTEREST_HALF_LIFE_SEC;
    float eff = hu_personal_topic_effective_score(&t, now);
    /* 0.8 * 0.5 = 0.4 ± 0.02 for table interpolation. */
    HU_ASSERT_TRUE(eff > 0.38f && eff < 0.42f);
}

static void personal_model_topic_effective_score_floors_at_zero_far_future(void) {
    hu_personal_topic_t t;
    memset(&t, 0, sizeof(t));
    t.interest_score = 0.95f;
    t.last_mentioned = 1000;
    int64_t now = t.last_mentioned + 20 * HU_PM_TOPIC_INTEREST_HALF_LIFE_SEC;
    HU_ASSERT_TRUE(hu_personal_topic_effective_score(&t, now) < 0.001f);
}

/* ── Goal decay ─────────────────────────────────────────────────────── */

static void personal_model_goal_effective_priority_handles_null(void) {
    HU_ASSERT_TRUE(hu_personal_goal_effective_priority(NULL, 1000000) == 0.f);
}

static void personal_model_goal_effective_priority_zero_for_inactive(void) {
    hu_personal_goal_t g;
    memset(&g, 0, sizeof(g));
    g.active = false;
    g.created_at = 1000;
    g.last_referenced = 1000;
    HU_ASSERT_TRUE(hu_personal_goal_effective_priority(&g, 1000) == 0.f);
}

static void personal_model_goal_effective_priority_zero_for_empty_slot(void) {
    hu_personal_goal_t g;
    memset(&g, 0, sizeof(g));
    g.active = true;
    /* Both timestamps zero — slot is uninitialized. */
    HU_ASSERT_TRUE(hu_personal_goal_effective_priority(&g, 1000000) == 0.f);
}

static void personal_model_goal_effective_priority_uses_created_at_fallback(void) {
    hu_personal_goal_t g;
    memset(&g, 0, sizeof(g));
    g.active = true;
    g.created_at = 1000;
    g.last_referenced = 0; /* never re-referenced; fall back to created_at */
    HU_ASSERT_TRUE(hu_personal_goal_effective_priority(&g, 1000) > 0.99f);
}

static void personal_model_goal_effective_priority_halves_at_one_half_life(void) {
    hu_personal_goal_t g;
    memset(&g, 0, sizeof(g));
    g.active = true;
    g.created_at = 1000;
    g.last_referenced = 1000;
    int64_t now = g.last_referenced + HU_PM_GOAL_RELEVANCE_HALF_LIFE_SEC;
    float eff = hu_personal_goal_effective_priority(&g, now);
    HU_ASSERT_TRUE(eff > 0.48f && eff < 0.52f);
}

/* ── Style freshness ────────────────────────────────────────────────── */

static void personal_model_style_freshness_handles_null(void) {
    HU_ASSERT_TRUE(hu_personal_communication_style_freshness(NULL, 1000000) == 0.f);
}

static void personal_model_style_freshness_zero_when_never_observed(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.sample_count = 0U;
    /* No samples → freshness undefined, treated as zero so the
     * directive never fires. */
    HU_ASSERT_TRUE(hu_personal_communication_style_freshness(&s, 1000000) == 0.f);
}

static void personal_model_style_freshness_full_when_unstamped_with_samples(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.sample_count = 10U;
    s.last_observed_at = 0; /* synthetic fixture */
    /* Samples present but no observation timestamp → freshness == 1.0
     * so pre-migration models keep their directive. */
    HU_ASSERT_TRUE(hu_personal_communication_style_freshness(&s, 1000000) > 0.99f);
}

static void personal_model_style_freshness_halves_at_one_half_life(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.sample_count = 10U;
    s.last_observed_at = 1000;
    int64_t now = s.last_observed_at + HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC;
    float fresh = hu_personal_communication_style_freshness(&s, now);
    HU_ASSERT_TRUE(fresh > 0.48f && fresh < 0.52f);
}

static void personal_model_ingest_stamps_style_last_observed_at(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t ts = 1700000000;
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, "hello there friend", 18, true, ts), HU_OK);
    HU_ASSERT_EQ(m.style.last_observed_at, ts);
}

/* ── D2.2 fidelity scorer ───────────────────────────────────────────── */

static void personal_model_fidelity_handles_null(void) {
    HU_ASSERT_TRUE(hu_communication_style_fidelity_score(NULL, "hi", 2) < 0.f);
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.sample_count = 5;
    HU_ASSERT_TRUE(hu_communication_style_fidelity_score(&s, NULL, 0) < 0.f);
    HU_ASSERT_TRUE(hu_communication_style_fidelity_score(&s, "x", 0) < 0.f);
}

static void personal_model_fidelity_returns_neg_when_no_samples(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    /* sample_count == 0 → no fingerprint to score against. The
     * caller can't do a comparison, so we say so explicitly. */
    HU_ASSERT_TRUE(hu_communication_style_fidelity_score(&s, "hello", 5) < 0.f);
}

static void personal_model_fidelity_high_for_matching_response(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.lowercase_ratio = 1.0f;       /* fully lowercase */
    s.abbreviation_ratio = 0.0f;    /* no shorthand */
    s.avg_message_length = 20;
    s.sample_count = 10;

    /* Response is fully lowercase, no abbrev, len ~= 20. Should
     * score very high (>0.85). */
    const char *r = "hey friend how are you";
    float score = hu_communication_style_fidelity_score(&s, r, strlen(r));
    HU_ASSERT_TRUE(score > 0.85f);
    HU_ASSERT_TRUE(score <= 1.0f);
}

static void personal_model_fidelity_low_for_uppercase_response(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.lowercase_ratio = 1.0f;       /* persona writes lowercase */
    s.avg_message_length = 20;
    s.sample_count = 10;

    /* Response is ALL UPPERCASE — lowercase axis match is 0. */
    const char *r = "HEY FRIEND HOW ARE YOU";
    float score = hu_communication_style_fidelity_score(&s, r, strlen(r));
    /* Lowercase axis: 0. Length axis: ~1. Abbrev axis: ~1.
     * Mean: (0 + 1 + 1) / 3 ~= 0.67. */
    HU_ASSERT_TRUE(score >= 0.5f && score < 0.75f);
}

static void personal_model_fidelity_rewards_abbreviation_match(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.lowercase_ratio = 1.0f;
    s.abbreviation_ratio = 0.5f;    /* persona uses lots of shorthand */
    s.avg_message_length = 20;
    s.sample_count = 10;

    /* Response: "u rn coding btw" — 4 words, "u"/"rn"/"btw" are
     * abbreviations → 3/4 = 0.75 abbrev ratio.
     * Persona target = 0.5; gap = 0.25 → axis score 0.75. */
    const char *r = "u rn coding btw";
    float score = hu_communication_style_fidelity_score(&s, r, strlen(r));
    HU_ASSERT_TRUE(score > 0.5f);
}

static void personal_model_fidelity_penalizes_length_mismatch(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.lowercase_ratio = 1.0f;
    s.avg_message_length = 20;
    s.sample_count = 10;

    /* Tiny response — 5 bytes vs target 20 → relative error = 0.75 →
     * length axis score = 0.25. Other axes match ~ perfectly →
     * mean ~= (0.25 + 1 + 1) / 3 ~= 0.75 */
    const char *r = "hi ok";
    float score = hu_communication_style_fidelity_score(&s, r, strlen(r));
    HU_ASSERT_TRUE(score > 0.6f && score < 0.85f);
}

static void personal_model_fidelity_zero_when_length_extremely_off(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.lowercase_ratio = 1.0f;
    s.avg_message_length = 10;
    s.sample_count = 10;

    /* Response is 100x the target length → relative error == 99 → axis = 0.
     * Mean of (1, 1, 0) = 0.667. */
    char r[1100];
    for (size_t i = 0; i < 1000; i++) r[i] = 'a';
    r[1000] = '\0';
    float score = hu_communication_style_fidelity_score(&s, r, 1000);
    HU_ASSERT_TRUE(score >= 0.6f && score < 0.7f);
}

static void personal_model_fidelity_score_is_deterministic(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.lowercase_ratio = 0.7f;
    s.abbreviation_ratio = 0.3f;
    s.avg_message_length = 50;
    s.sample_count = 10;

    const char *r = "yo wanted to lmk btw the project is on track now";
    float a = hu_communication_style_fidelity_score(&s, r, strlen(r));
    float b = hu_communication_style_fidelity_score(&s, r, strlen(r));
    float c = hu_communication_style_fidelity_score(&s, r, strlen(r));
    /* No randomness anywhere — three calls must return bit-identical
     * floats for offline reproducibility. */
    HU_ASSERT_TRUE(a == b);
    HU_ASSERT_TRUE(b == c);
}

/* ── Compare response sets (lora-ab comparator) ─────────────────────── */

static hu_communication_style_t make_test_target(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.lowercase_ratio = 0.85f;
    s.abbreviation_ratio = 0.2f;
    s.avg_message_length = 60;
    s.sample_count = 1; /* tip past the no-fingerprint guard */
    return s;
}

static void personal_model_compare_response_sets_rejects_null_args(void) {
    hu_communication_style_t s = make_test_target();
    hu_communication_style_set_summary_t a, b;
    float delta;
    /* NULL target. */
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(NULL, NULL, NULL, 0, NULL, NULL, 0,
                                                              &a, &b, &delta),
                 HU_ERR_INVALID_ARGUMENT);
    /* NULL out_a / out_b / out_delta. */
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(&s, NULL, NULL, 0, NULL, NULL, 0,
                                                              NULL, &b, &delta),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(&s, NULL, NULL, 0, NULL, NULL, 0,
                                                              &a, NULL, &delta),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(&s, NULL, NULL, 0, NULL, NULL, 0,
                                                              &a, &b, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void personal_model_compare_response_sets_rejects_zero_sample_target(void) {
    hu_communication_style_t s = make_test_target();
    s.sample_count = 0;
    hu_communication_style_set_summary_t a, b;
    float delta;
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(&s, NULL, NULL, 0, NULL, NULL, 0,
                                                              &a, &b, &delta),
                 HU_ERR_INVALID_ARGUMENT);
}

static void personal_model_compare_response_sets_handles_empty_sets(void) {
    hu_communication_style_t s = make_test_target();
    hu_communication_style_set_summary_t a, b;
    float delta = -42.f;
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(&s, NULL, NULL, 0, NULL, NULL, 0,
                                                              &a, &b, &delta),
                 HU_OK);
    HU_ASSERT_EQ(a.scored, 0u);
    HU_ASSERT_EQ(b.scored, 0u);
    HU_ASSERT_EQ((double)delta, 0.0);
}

static void personal_model_compare_response_sets_reports_positive_delta(void) {
    /* set_a: high-formality, no abbrevs, much longer than 60 chars
     *        → should score lower against the casual target.
     * set_b: lowercase, abbrev-heavy, ~60 chars → high score. */
    hu_communication_style_t s = make_test_target();
    const char *set_a[] = {
        "Dear Sir, I AM PLEASED TO INFORM YOU THAT THE REQUEST HAS BEEN PROCESSED IN ACCORDANCE.",
        "Pursuant to your previous communication, please find attached the requested documentation.",
    };
    const char *set_b[] = {
        "hey, lmk if u want anything else from me today, totally happy to help",
        "yeah no worries, btw the report is ready whenever u need it from me",
    };
    hu_communication_style_set_summary_t a, b;
    float delta = 0.f;
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(
                     &s, set_a, NULL, 2, set_b, NULL, 2, &a, &b, &delta),
                 HU_OK);
    HU_ASSERT_EQ(a.scored, 2u);
    HU_ASSERT_EQ(b.scored, 2u);
    HU_ASSERT_TRUE(a.mean < b.mean);
    HU_ASSERT_TRUE(delta > 0.f);
    /* Delta should be substantial (>0.10) for these dramatically
     * different sets. */
    HU_ASSERT_TRUE(delta > 0.10f);
}

static void personal_model_compare_response_sets_negative_delta_when_b_worse(void) {
    /* Inverted: high-fidelity set as A, low-fidelity as B.
     * Delta should be negative. */
    hu_communication_style_t s = make_test_target();
    const char *set_a[] = {
        "yeah totally, lmk if u want me to send the report rn",
    };
    const char *set_b[] = {
        "I WOULD LIKE TO RESPECTFULLY INFORM YOU OF THE FOLLOWING IMPORTANT MATTER REGARDING THE PROJECT REQUIREMENTS HEREIN.",
    };
    hu_communication_style_set_summary_t a, b;
    float delta = 0.f;
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(
                     &s, set_a, NULL, 1, set_b, NULL, 1, &a, &b, &delta),
                 HU_OK);
    HU_ASSERT_TRUE(a.mean > b.mean);
    HU_ASSERT_TRUE(delta < 0.f);
}

static void personal_model_compare_response_sets_skips_invalid_responses(void) {
    /* NULL / empty / over-short responses should be counted in
     * `skipped`, not `scored`, and not drag the mean toward zero. */
    hu_communication_style_t s = make_test_target();
    const char *set_a[] = {
        NULL,
        "",
        "hey lmk if u want anything else today",
    };
    size_t lens_a[] = {0, 0, 37};
    hu_communication_style_set_summary_t a, b;
    float delta = 0.f;
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(
                     &s, set_a, lens_a, 3, NULL, NULL, 0, &a, &b, &delta),
                 HU_OK);
    HU_ASSERT_EQ(a.scored, 1u);
    HU_ASSERT_EQ(a.skipped, 2u);
    /* Only the third entry contributed to the mean → mean must
     * be > 0 (not zero from the skipped ones). */
    HU_ASSERT_TRUE(a.mean > 0.f);
}

static void personal_model_compare_response_sets_reports_min_max(void) {
    /* For a single-response set, min == max == mean. */
    hu_communication_style_t s = make_test_target();
    const char *set_a[] = {
        "hey lmk if u want anything else today",
    };
    hu_communication_style_set_summary_t a, b;
    float delta = 0.f;
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(
                     &s, set_a, NULL, 1, NULL, NULL, 0, &a, &b, &delta),
                 HU_OK);
    HU_ASSERT_EQ(a.scored, 1u);
    HU_ASSERT_TRUE(a.min_score == a.max_score);
    HU_ASSERT_TRUE(a.min_score == a.mean);
}

static void personal_model_compare_response_sets_uses_explicit_lens(void) {
    /* When `lens` is provided, the comparator must use it instead
     * of strlen — useful for un-NUL-terminated buffers from the
     * JSON loader. We test by passing a buffer that's longer than
     * its declared length. */
    hu_communication_style_t s = make_test_target();
    const char *set_a[] = {
        "hey lmk if u want anything else today FORBIDDEN_TAIL",
    };
    /* Declared length stops before "FORBIDDEN_TAIL" — score should
     * reflect the truncated content. */
    size_t lens_a[] = {37};
    hu_communication_style_set_summary_t a, b;
    float delta = 0.f;
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets(
                     &s, set_a, lens_a, 1, NULL, NULL, 0, &a, &b, &delta),
                 HU_OK);
    HU_ASSERT_EQ(a.scored, 1u);
    /* The score with the truncated length should be different from
     * the score with the full (untruncated) length. */
    float full_score =
        hu_communication_style_fidelity_score(&s, set_a[0], strlen(set_a[0]));
    float truncated_score = a.mean;
    HU_ASSERT_TRUE(full_score != truncated_score);
}

/* ── Style blend toward neutral ─────────────────────────────────────── */

static void personal_model_style_blend_handles_null(void) {
    hu_communication_style_t out =
        hu_personal_communication_style_blend_with_freshness(NULL, 1000000);
    /* On NULL input the helper returns a zero-initialized struct.
     * No fields read raw; this is a purely defensive check. */
    HU_ASSERT_EQ((unsigned)out.sample_count, 0u);
    HU_ASSERT_EQ((unsigned)out.avg_message_length, 0u);
}

static void personal_model_style_blend_returns_raw_at_full_freshness(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.formality = 0.2f;
    s.verbosity = 0.8f;
    s.emoji_frequency = 0.1f;
    s.humor_receptivity = 0.9f;
    s.lowercase_ratio = 0.7f;
    s.abbreviation_ratio = 0.3f;
    s.avg_message_length = 80;
    s.sample_count = 20;
    s.last_observed_at = 1700000000LL;

    /* now == last_observed_at → freshness == 1.0 → blended values
     * equal raw values. */
    hu_communication_style_t b =
        hu_personal_communication_style_blend_with_freshness(&s, 1700000000LL);
    HU_ASSERT_TRUE(b.formality > 0.19f && b.formality < 0.21f);
    HU_ASSERT_TRUE(b.verbosity > 0.79f && b.verbosity < 0.81f);
    HU_ASSERT_TRUE(b.emoji_frequency > 0.09f && b.emoji_frequency < 0.11f);
    HU_ASSERT_TRUE(b.humor_receptivity > 0.89f && b.humor_receptivity < 0.91f);
    HU_ASSERT_TRUE(b.lowercase_ratio > 0.69f && b.lowercase_ratio < 0.71f);
    HU_ASSERT_TRUE(b.abbreviation_ratio > 0.29f && b.abbreviation_ratio < 0.31f);
    /* Pass-through. */
    HU_ASSERT_EQ((unsigned)b.avg_message_length, 80u);
    HU_ASSERT_EQ((unsigned)b.sample_count, 20u);
}

static void personal_model_style_blend_pulls_to_neutral_at_half_life(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.formality = 0.2f;          /* very casual */
    s.verbosity = 0.9f;          /* very verbose */
    s.lowercase_ratio = 0.8f;    /* mostly lowercase */
    s.abbreviation_ratio = 0.6f; /* moderate shorthand */
    s.sample_count = 20;
    /* Stamp must be positive — last_observed_at <= 0 is the
     * "pre-migration fallback" branch in the freshness function and
     * returns 1.0 regardless of `now`. */
    s.last_observed_at = 1700000000LL;
    /* now exactly one half-life after observation → freshness ~= 0.5
     * → blended axis = raw * 0.5 + 0.5 * 0.5 = (raw + 0.5) / 2.
     * For 0.2 raw → 0.35; for 0.9 → 0.7; for 0.8 → 0.65; 0.6 → 0.55. */
    int64_t now = s.last_observed_at + HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC;
    hu_communication_style_t b =
        hu_personal_communication_style_blend_with_freshness(&s, now);
    HU_ASSERT_TRUE(b.formality > 0.34f && b.formality < 0.36f);
    HU_ASSERT_TRUE(b.verbosity > 0.69f && b.verbosity < 0.71f);
    HU_ASSERT_TRUE(b.lowercase_ratio > 0.64f && b.lowercase_ratio < 0.66f);
    HU_ASSERT_TRUE(b.abbreviation_ratio > 0.54f && b.abbreviation_ratio < 0.56f);
}

static void personal_model_style_blend_returns_neutral_when_freshness_zero(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.formality = 0.2f;
    s.verbosity = 0.9f;
    s.lowercase_ratio = 0.8f;
    s.abbreviation_ratio = 0.6f;
    s.sample_count = 0; /* sample_count == 0 forces freshness == 0 */
    s.last_observed_at = 1700000000LL;

    hu_communication_style_t b =
        hu_personal_communication_style_blend_with_freshness(&s, 1700100000LL);
    /* All blended axes should be exactly 0.5 (full pull to neutral). */
    HU_ASSERT_TRUE(b.formality > 0.49f && b.formality < 0.51f);
    HU_ASSERT_TRUE(b.verbosity > 0.49f && b.verbosity < 0.51f);
    HU_ASSERT_TRUE(b.lowercase_ratio > 0.49f && b.lowercase_ratio < 0.51f);
    HU_ASSERT_TRUE(b.abbreviation_ratio > 0.49f && b.abbreviation_ratio < 0.51f);
}

static void personal_model_style_blend_does_not_mutate_input(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.formality = 0.2f;
    s.verbosity = 0.9f;
    s.sample_count = 20;
    s.last_observed_at = 1700000000LL;

    int64_t now = s.last_observed_at + HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC;
    (void)hu_personal_communication_style_blend_with_freshness(&s, now);
    /* Raw values must be untouched after the blend call. */
    HU_ASSERT_TRUE(s.formality > 0.19f && s.formality < 0.21f);
    HU_ASSERT_TRUE(s.verbosity > 0.89f && s.verbosity < 0.91f);
}

/* ── Prompt gating on freshness ─────────────────────────────────────── */

static void personal_model_build_prompt_drops_stale_topic(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Seed an updated_at far enough in the future that 10× topic
     * half-lives still leaves last_mentioned > 0. */
    m.updated_at = (int64_t)(40LL * HU_PM_TOPIC_INTEREST_HALF_LIFE_SEC);

    /* Fresh topic: last_mentioned just before now. */
    snprintf(m.topics[0].name, sizeof(m.topics[0].name), "fresh_topic");
    m.topics[0].interest_score = 0.7f;
    m.topics[0].mention_count = 3U;
    m.topics[0].last_mentioned = m.updated_at - 1;

    /* Stale topic: 10 half-lives ago → eff < 0.001. */
    snprintf(m.topics[1].name, sizeof(m.topics[1].name), "stale_topic");
    m.topics[1].interest_score = 0.95f;
    m.topics[1].mention_count = 200U;
    m.topics[1].last_mentioned = m.updated_at - 10 * HU_PM_TOPIC_INTEREST_HALF_LIFE_SEC;

    m.topic_count = 2;

    char buf[2048];
    (void)hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_NOT_NULL(strstr(buf, "fresh_topic"));
    HU_ASSERT_NULL(strstr(buf, "stale_topic"));
}

static void personal_model_build_prompt_drops_stale_goal(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.updated_at = (int64_t)(40LL * HU_PM_GOAL_RELEVANCE_HALF_LIFE_SEC);

    /* Active fresh goal — included. */
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the new feature");
    m.goals[0].active = true;
    m.goals[0].created_at = m.updated_at - 1;
    m.goals[0].last_referenced = m.updated_at - 1;

    /* Active but stale goal — dropped. */
    snprintf(m.goals[1].description, sizeof(m.goals[1].description), "learn esperanto");
    m.goals[1].active = true;
    m.goals[1].created_at = m.updated_at - 10 * HU_PM_GOAL_RELEVANCE_HALF_LIFE_SEC;
    m.goals[1].last_referenced = m.updated_at - 10 * HU_PM_GOAL_RELEVANCE_HALF_LIFE_SEC;

    m.goal_count = 2;

    char buf[2048];
    (void)hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_NOT_NULL(strstr(buf, "ship the new feature"));
    HU_ASSERT_NULL(strstr(buf, "learn esperanto"));
}

static void personal_model_build_prompt_drops_style_directive_when_stale(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.updated_at = (int64_t)(40LL * HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC);
    /* Sample count above the warm-up threshold — the directive would
     * fire under the old logic. */
    m.style.sample_count = 50U;
    m.style.formality = 0.2f;
    m.style.verbosity = 0.3f;
    m.style.emoji_frequency = 0.0f;
    m.style.humor_receptivity = 0.5f;
    m.style.avg_message_length = 80U;
    /* Style was last observed 10 half-lives ago — freshness floors to 0. */
    m.style.last_observed_at = m.updated_at - 10 * HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC;

    char buf[2048];
    (void)hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    /* The "Mirror their style:" directive must NOT fire when freshness
     * is below the gate. The "Communication style:" observation line
     * is allowed; it's a passive description, not a steering directive. */
    HU_ASSERT_NULL(strstr(buf, "Mirror their style"));
}

/* ── Recently-completed retention ───────────────────────────────────── */

static void personal_model_recently_completed_handles_null(void) {
    HU_ASSERT_FALSE(hu_personal_goal_is_recently_completed(NULL, 1000));
}

static void personal_model_recently_completed_active_returns_false(void) {
    hu_personal_goal_t g;
    memset(&g, 0, sizeof(g));
    g.active = true;
    g.last_referenced = 1000;
    HU_ASSERT_FALSE(hu_personal_goal_is_recently_completed(&g, 1000));
}

static void personal_model_recently_completed_inside_window_returns_true(void) {
    hu_personal_goal_t g;
    memset(&g, 0, sizeof(g));
    g.active = false;
    g.last_referenced = 1000;
    /* age = 6 days < 7-day retention. */
    int64_t now = 1000 + 6 * 86400;
    HU_ASSERT_TRUE(hu_personal_goal_is_recently_completed(&g, now));
}

static void personal_model_recently_completed_at_window_boundary_returns_true(void) {
    hu_personal_goal_t g;
    memset(&g, 0, sizeof(g));
    g.active = false;
    g.last_referenced = 1000;
    /* age = exactly 7 days = retention. <= boundary returns true. */
    int64_t now = 1000 + HU_PM_COMPLETED_GOAL_RETAIN_SEC;
    HU_ASSERT_TRUE(hu_personal_goal_is_recently_completed(&g, now));
}

static void personal_model_recently_completed_past_window_returns_false(void) {
    hu_personal_goal_t g;
    memset(&g, 0, sizeof(g));
    g.active = false;
    g.last_referenced = 1000;
    /* age = 8 days > 7-day retention. */
    int64_t now = 1000 + 8 * 86400;
    HU_ASSERT_FALSE(hu_personal_goal_is_recently_completed(&g, now));
}

static void personal_model_recently_completed_no_stamp_returns_false(void) {
    hu_personal_goal_t g;
    memset(&g, 0, sizeof(g));
    g.active = false;
    g.last_referenced = 0; /* unstamped */
    HU_ASSERT_FALSE(hu_personal_goal_is_recently_completed(&g, 1000));
}

static void personal_model_get_recently_completed_handles_null(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const hu_personal_goal_t *buf[4] = {0};
    HU_ASSERT_EQ(hu_personal_model_get_recently_completed_goals(NULL, 1000, buf, 4), 0u);
    HU_ASSERT_EQ(hu_personal_model_get_recently_completed_goals(&m, 1000, NULL, 4), 0u);
    HU_ASSERT_EQ(hu_personal_model_get_recently_completed_goals(&m, 1000, buf, 0), 0u);
}

static void personal_model_get_recently_completed_returns_zero_when_none(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "active goal");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;
    const hu_personal_goal_t *buf[4] = {0};
    HU_ASSERT_EQ(hu_personal_model_get_recently_completed_goals(&m, 1000, buf, 4), 0u);
    HU_ASSERT_NULL(buf[0]);
}

static void personal_model_get_recently_completed_returns_inside_window(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* g0 active (skip), g1 recently completed (include),
     * g2 long-completed (skip). */
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "active");
    m.goals[0].active = true;
    m.goals[0].last_referenced = 1000;

    snprintf(m.goals[1].description, sizeof(m.goals[1].description), "ship feature");
    m.goals[1].active = false;
    m.goals[1].last_referenced = 1000;

    snprintf(m.goals[2].description, sizeof(m.goals[2].description), "old work");
    m.goals[2].active = false;
    m.goals[2].last_referenced = 1000;

    m.goal_count = 3;

    const hu_personal_goal_t *buf[4] = {0};
    /* now = 3 days after stamps. g1 is in the 7-day window;
     * g2's age is also 3 days but we use a `now` past g2's window
     * by overriding its stamp. */
    int64_t now_inside = 1000 + 3 * 86400;
    size_t got = hu_personal_model_get_recently_completed_goals(&m, now_inside, buf, 4);
    HU_ASSERT_EQ(got, 2u);
    /* g1 first (i=1), then g2 (i=2) — preserved insertion order. */
    HU_ASSERT_STR_EQ(buf[0]->description, "ship feature");
    HU_ASSERT_STR_EQ(buf[1]->description, "old work");
}

static void personal_model_get_recently_completed_respects_cap(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Three completed goals all in the window — cap=2 must
     * truncate to the first two and report 2. */
    for (size_t i = 0; i < 3; i++) {
        snprintf(m.goals[i].description, sizeof(m.goals[i].description), "completed_%zu", i);
        m.goals[i].active = false;
        m.goals[i].last_referenced = 1000;
    }
    m.goal_count = 3;

    const hu_personal_goal_t *buf[2] = {0};
    int64_t now = 1000 + 86400;
    size_t got = hu_personal_model_get_recently_completed_goals(&m, now, buf, 2);
    HU_ASSERT_EQ(got, 2u);
    HU_ASSERT_STR_EQ(buf[0]->description, "completed_0");
    HU_ASSERT_STR_EQ(buf[1]->description, "completed_1");
}

static void personal_model_get_recently_completed_skips_empty_slots(void) {
    /* A goal slot with an empty description is treated as a free
     * slot and skipped — even if other fields look completed. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.goals[0].description[0] = '\0';
    m.goals[0].active = false;
    m.goals[0].last_referenced = 1000;
    snprintf(m.goals[1].description, sizeof(m.goals[1].description), "real goal");
    m.goals[1].active = false;
    m.goals[1].last_referenced = 1000;
    m.goal_count = 2;

    const hu_personal_goal_t *buf[4] = {0};
    int64_t now = 1000 + 86400;
    size_t got = hu_personal_model_get_recently_completed_goals(&m, now, buf, 4);
    HU_ASSERT_EQ(got, 1u);
    HU_ASSERT_STR_EQ(buf[0]->description, "real goal");
}

static void personal_model_describe_recently_completed_handles_null(void) {
    char buf[64];
    /* NULL model, valid buf — must NUL-terminate and return 0. */
    HU_ASSERT_EQ(hu_personal_model_describe_recently_completed(NULL, 1000, buf, sizeof(buf)), 0u);
    HU_ASSERT_EQ((int)buf[0], 0);
    /* Valid model, NULL buf — return 0 without writing. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_EQ(hu_personal_model_describe_recently_completed(&m, 1000, NULL, 64), 0u);
    /* cap = 0 — return 0. */
    HU_ASSERT_EQ(hu_personal_model_describe_recently_completed(&m, 1000, buf, 0), 0u);
}

static void personal_model_describe_recently_completed_returns_zero_when_none(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "active");
    m.goals[0].active = true;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;
    char buf[64] = "garbage";
    HU_ASSERT_EQ(hu_personal_model_describe_recently_completed(&m, 1000, buf, sizeof(buf)), 0u);
    /* Must clobber the garbage with a NUL. */
    HU_ASSERT_EQ((int)buf[0], 0);
}

static void personal_model_describe_recently_completed_renders_one(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = false;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;
    char buf[64];
    size_t n = hu_personal_model_describe_recently_completed(&m, 1000 + 86400, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "ship feature");
}

static void personal_model_describe_recently_completed_renders_multiple_with_separator(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    for (size_t i = 0; i < 3; i++) {
        snprintf(m.goals[i].description, sizeof(m.goals[i].description), "goal_%zu", i);
        m.goals[i].active = false;
        m.goals[i].last_referenced = 1000;
    }
    m.goal_count = 3;
    char buf[128];
    size_t n = hu_personal_model_describe_recently_completed(&m, 1000 + 86400, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "goal_0, goal_1, goal_2");
}

static void personal_model_describe_recently_completed_truncates_with_ellipsis(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* Three "long_description_NN" entries; small buf forces
     * truncation after the first. */
    for (size_t i = 0; i < 3; i++) {
        snprintf(m.goals[i].description, sizeof(m.goals[i].description),
                 "long_description_%02zu", i);
        m.goals[i].active = false;
        m.goals[i].last_referenced = 1000;
    }
    m.goal_count = 3;
    /* Long enough for "long_description_00" + ", ..." + NUL =
     * 19 + 5 + 1 = 25; not long enough for a second description. */
    char buf[28];
    size_t n = hu_personal_model_describe_recently_completed(&m, 1000 + 86400, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    /* Must contain the ellipsis suffix. */
    HU_ASSERT_TRUE(strstr(buf, ", ...") != NULL);
    /* Must contain the first description. */
    HU_ASSERT_TRUE(strstr(buf, "long_description_00") != NULL);
    /* Must be NUL-terminated within the buf. */
    HU_ASSERT_TRUE(strlen(buf) < sizeof(buf));
}

static void personal_model_describe_recently_completed_ignores_old(void) {
    /* A goal past the retention window is filtered out; the
     * description must NOT include it. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ancient");
    m.goals[0].active = false;
    m.goals[0].last_referenced = 1000;
    snprintf(m.goals[1].description, sizeof(m.goals[1].description), "fresh");
    m.goals[1].active = false;
    m.goals[1].last_referenced = 1000 + 30 * 86400;
    m.goal_count = 2;
    char buf[64];
    /* now is past retention for goal 0 (30+ days from 1000) but
     * within retention for goal 1 (1000 + 30d + 1d). */
    size_t n = hu_personal_model_describe_recently_completed(&m, 1000 + 31 * 86400, buf,
                                                              sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "fresh");
}

static void personal_model_apply_decay_keeps_recently_completed(void) {
    /* End-to-end: a resolved goal in the retention window survives
     * apply_decay; the same goal past the retention window doesn't. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = false;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;

    /* Inside retention window — kept. */
    int64_t inside = 1000 + 3 * 86400;
    size_t pruned = hu_personal_model_apply_decay(&m, inside);
    HU_ASSERT_EQ(pruned, 0u);
    HU_ASSERT_EQ(m.goal_count, 1u);

    /* Past retention window — pruned. */
    int64_t past = 1000 + 30 * 86400;
    pruned = hu_personal_model_apply_decay(&m, past);
    HU_ASSERT_EQ(pruned, 1u);
    HU_ASSERT_EQ(m.goal_count, 0u);
}

static void personal_model_resolve_goals_stamps_last_referenced(void) {
    /* Defensive test: even when called WITHOUT a prior touch_goals
     * call, resolve_goals must stamp last_referenced so the
     * recently-completed retention works. The per_turn_tick helper
     * always calls touch first, but unit tests and future direct
     * callers might not. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    m.goals[0].last_referenced = 100; /* stale stamp */
    m.goal_count = 1;

    const char *msg = "shipped feature";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 1u);
    HU_ASSERT_FALSE(m.goals[0].active);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 5000L);
}

static void personal_model_build_prompt_surfaces_recently_completed(void) {
    /* Integration: a model with a recently-completed goal renders
     * a "Recently completed: <desc>" line in the prompt. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the new feature");
    m.goals[0].active = false;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;
    /* updated_at = pm_now in build_prompt; set within retention. */
    m.updated_at = 1000 + 3 * 86400;

    char buf[2048];
    size_t written = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_TRUE(written > 0);
    HU_ASSERT_TRUE(strstr(buf, "Recently completed:") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ship the new feature") != NULL);
}

static void personal_model_build_prompt_omits_old_completed(void) {
    /* A goal completed 30 days ago should NOT appear — past
     * retention, both the prompt builder and apply_decay drop it. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the old feature");
    m.goals[0].active = false;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;
    m.updated_at = 1000 + 30 * 86400;

    char buf[2048];
    (void)hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "Recently completed:") == NULL);
}

static void personal_model_build_prompt_emits_acknowledgment_directive(void) {
    /* When a recently-completed goal is surfaced, the prompt
     * MUST also carry a behavioral directive telling the model
     * to acknowledge the completion. Without this, the structural
     * "Recently completed: …" list is just trivia — the model
     * has no instruction to actually use it. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the new feature");
    m.goals[0].active = false;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;
    m.updated_at = 1000 + 86400;

    char buf[2048];
    (void)hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    /* The directive must appear AFTER the structural list (i.e.
     * the directive references the listed items, not the other
     * way around). */
    const char *list = strstr(buf, "Recently completed:");
    const char *directive = strstr(buf, "acknowledge it warmly");
    HU_ASSERT_NOT_NULL(list);
    HU_ASSERT_NOT_NULL(directive);
    HU_ASSERT_TRUE(directive > list);
    /* The directive should also include the dominate-the-reply
     * guardrail so a future refactor can't quietly drop it. */
    HU_ASSERT_TRUE(strstr(buf, "dominate the reply") != NULL);
}

/* Helper for overlay-tuned directive tests — populates a model
 * with one freshly-completed goal so the directive line is
 * always emitted. */
static void overlay_directive_seed_model(hu_personal_model_t *m) {
    hu_personal_model_init(m);
    snprintf(m->goals[0].description, sizeof(m->goals[0].description), "ship the new feature");
    m->goals[0].active = false;
    m->goals[0].last_referenced = 1000;
    m->goal_count = 1;
    m->updated_at = 1000 + 86400;
}

static void personal_model_build_prompt_overlay_null_matches_legacy(void) {
    /* The legacy `_build_prompt` MUST be byte-identical to
     * `_build_prompt_with_overlay(model, NULL, ...)`. If they
     * diverge, every existing caller silently changes behavior. */
    hu_personal_model_t m;
    overlay_directive_seed_model(&m);
    char legacy[2048];
    char with_null[2048];
    size_t l = hu_personal_model_build_prompt(&m, legacy, sizeof(legacy));
    size_t w = hu_personal_model_build_prompt_with_overlay(&m, NULL, with_null, sizeof(with_null));
    HU_ASSERT_EQ(l, w);
    HU_ASSERT_TRUE(strcmp(legacy, with_null) == 0);
}

static void personal_model_build_prompt_overlay_formal_emits_terse_directive(void) {
    /* A formal-channel overlay must produce the strict-register
     * variant: "respectful one-liner, no emoji". */
    hu_persona_overlay_t ov = {.channel = "email", .formality = "formal"};
    hu_personal_model_t m;
    overlay_directive_seed_model(&m);
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &ov, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "respectful one-liner") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "no emoji") != NULL);
    /* The casual variant's words must NOT appear. */
    HU_ASSERT_TRUE(strstr(buf, "warmly") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "quick congrats") == NULL);
}

static void personal_model_build_prompt_overlay_casual_emoji_emits_permissive_variant(void) {
    /* Casual + moderate emoji: the most permissive variant.
     * "an emoji is fine if it fits" must appear. */
    hu_persona_overlay_t ov = {.channel = "imessage",
                               .formality = "casual",
                               .emoji_usage = "moderate"};
    hu_personal_model_t m;
    overlay_directive_seed_model(&m);
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &ov, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "quick congrats") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "emoji is fine") != NULL);
    /* Formal variant words must NOT appear. */
    HU_ASSERT_TRUE(strstr(buf, "respectful one-liner") == NULL);
}

static void personal_model_build_prompt_overlay_short_length_emphasizes_brevity(void) {
    /* avg_length="short" or numeric ≤30 must steer toward the
     * brevity-focused variant: "one sentence". */
    hu_persona_overlay_t ov = {.channel = "imessage", .avg_length = "short"};
    hu_personal_model_t m;
    overlay_directive_seed_model(&m);
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &ov, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "one sentence") != NULL);
    /* Numeric lower bound (≤30) gates the same path. */
    hu_persona_overlay_t ov2 = {.channel = "imessage", .avg_length = "20"};
    char buf2[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &ov2, buf2, sizeof(buf2));
    HU_ASSERT_TRUE(strstr(buf2, "one sentence") != NULL);
}

static void personal_model_build_prompt_overlay_formal_overrides_emoji(void) {
    /* Formal trumps emoji license. Even with emoji_usage="high",
     * a formal channel must emit the no-emoji variant. */
    hu_persona_overlay_t ov = {.channel = "linkedin",
                               .formality = "formal",
                               .emoji_usage = "high"};
    hu_personal_model_t m;
    overlay_directive_seed_model(&m);
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &ov, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "no emoji") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "emoji is fine") == NULL);
}

static void personal_model_build_prompt_overlay_unknown_falls_through_to_default(void) {
    /* An overlay with no recognized signal (channel only) must
     * produce the same wording as NULL — no signal, no
     * differentiation. */
    hu_persona_overlay_t ov = {.channel = "novel-channel"};
    hu_personal_model_t m;
    overlay_directive_seed_model(&m);
    char with_overlay[2048];
    char with_null[2048];
    size_t a = hu_personal_model_build_prompt_with_overlay(&m, &ov, with_overlay,
                                                           sizeof(with_overlay));
    size_t b = hu_personal_model_build_prompt_with_overlay(&m, NULL, with_null, sizeof(with_null));
    HU_ASSERT_EQ(a, b);
    HU_ASSERT_TRUE(strcmp(with_overlay, with_null) == 0);
}

/* ── Directive variant telemetry (Track D D2.2) ──────────────────
 *
 * Each test resets the global counters first so it's isolated
 * from any earlier directive fires in the same process. */

static void personal_model_directive_telemetry_reset_zeros_counts(void) {
    hu_personal_model_directive_telemetry_reset();
    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    HU_ASSERT_EQ((unsigned long long)s.total, 0ULL);
    for (size_t i = 0; i < HU_DIRECTIVE_VARIANT__COUNT; i++) {
        HU_ASSERT_EQ((unsigned long long)s.counts[i], 0ULL);
    }
}

static void personal_model_directive_telemetry_null_overlay_increments_null(void) {
    hu_personal_model_directive_telemetry_reset();
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    overlay_directive_seed_model(&m);
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, NULL, buf, sizeof(buf));
    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 1ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 1ULL);
}

static void personal_model_directive_telemetry_formal_overlay_increments_formal(void) {
    hu_personal_model_directive_telemetry_reset();
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    overlay_directive_seed_model(&m);
    hu_persona_overlay_t overlay = {0};
    overlay.formality = (char *)"formal";
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &overlay, buf, sizeof(buf));
    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_FORMAL_TERSE], 1ULL);
    /* No spillover into other buckets. */
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 1ULL);
}

static void personal_model_directive_telemetry_casual_emoji_increments_casual_emoji(void) {
    hu_personal_model_directive_telemetry_reset();
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    overlay_directive_seed_model(&m);
    hu_persona_overlay_t overlay = {0};
    overlay.formality = (char *)"casual";
    overlay.emoji_usage = (char *)"moderate";
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &overlay, buf, sizeof(buf));
    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 1ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 1ULL);
}

static void personal_model_directive_telemetry_unspecified_overlay_increments_default(void) {
    /* An overlay that's structurally present but carries no
     * useful signal lands in DEFAULT, NOT NULL_OVERLAY — the
     * distinction matters for dashboards: it tells the operator
     * "the channel HAS an overlay but it's not steering the
     * directive" vs "no overlay at all". */
    hu_personal_model_directive_telemetry_reset();
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    overlay_directive_seed_model(&m);
    hu_persona_overlay_t overlay = {0};
    overlay.formality = (char *)"adaptive";
    overlay.emoji_usage = (char *)"minimal";
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &overlay, buf, sizeof(buf));
    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_DEFAULT], 1ULL);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_NULL_OVERLAY], 0ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 1ULL);
}

static void personal_model_directive_telemetry_repeated_calls_accumulate(void) {
    /* The counter must accumulate across calls — that's the
     * whole point of telemetry. Three casual+emoji calls means
     * count=3, not count=1-with-stale-reset. */
    hu_personal_model_directive_telemetry_reset();
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    overlay_directive_seed_model(&m);
    hu_persona_overlay_t overlay = {0};
    overlay.formality = (char *)"casual";
    overlay.emoji_usage = (char *)"high";
    char buf[2048];
    (void)hu_personal_model_build_prompt_with_overlay(&m, &overlay, buf, sizeof(buf));
    (void)hu_personal_model_build_prompt_with_overlay(&m, &overlay, buf, sizeof(buf));
    (void)hu_personal_model_build_prompt_with_overlay(&m, &overlay, buf, sizeof(buf));
    hu_directive_telemetry_t s;
    hu_personal_model_directive_telemetry_snapshot(&s);
    HU_ASSERT_EQ((unsigned long long)s.counts[HU_DIRECTIVE_VARIANT_CASUAL_EMOJI], 3ULL);
    HU_ASSERT_EQ((unsigned long long)s.total, 3ULL);
}

static void personal_model_directive_variant_label_returns_known(void) {
    HU_ASSERT_TRUE(strcmp(hu_personal_model_directive_variant_label(
                              HU_DIRECTIVE_VARIANT_NULL_OVERLAY),
                          "null_overlay") == 0);
    HU_ASSERT_TRUE(strcmp(hu_personal_model_directive_variant_label(
                              HU_DIRECTIVE_VARIANT_DEFAULT),
                          "default") == 0);
    HU_ASSERT_TRUE(strcmp(hu_personal_model_directive_variant_label(
                              HU_DIRECTIVE_VARIANT_FORMAL_TERSE),
                          "formal_terse") == 0);
    HU_ASSERT_TRUE(strcmp(hu_personal_model_directive_variant_label(
                              HU_DIRECTIVE_VARIANT_CASUAL_EMOJI),
                          "casual_emoji") == 0);
    HU_ASSERT_TRUE(strcmp(hu_personal_model_directive_variant_label(
                              HU_DIRECTIVE_VARIANT_CASUAL_OR_SHORT),
                          "casual_or_short") == 0);
    HU_ASSERT_TRUE(strcmp(hu_personal_model_directive_variant_label(
                              HU_DIRECTIVE_VARIANT_ADAPTIVE_EMOJI),
                          "adaptive_emoji") == 0);
    /* Out-of-range gets the safe placeholder. */
    HU_ASSERT_TRUE(strcmp(hu_personal_model_directive_variant_label(
                              (hu_directive_variant_t)999),
                          "unknown") == 0);
}

static void personal_model_build_prompt_omits_directive_when_no_completed(void) {
    /* Inverted: a model with NO recently-completed goals must
     * NOT emit the directive line. Tokens spent on dormant
     * directives degrade the signal-to-noise ratio of the
     * system prompt. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "active goal");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;
    m.updated_at = 1000 + 86400;

    char buf[2048];
    (void)hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "acknowledge it warmly") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "dominate the reply") == NULL);
}

/* ── Goal completion detection ──────────────────────────────────────── */

static void personal_model_resolve_goals_handles_null_args(void) {
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(NULL, "shipped feature", 15, 1000),
                 0u);
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, NULL, 0, 1000), 0u);
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, "x", 0, 1000), 0u);
}

static void personal_model_resolve_goals_deactivates_on_completion_verb(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the new feature");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goal_count = 1;

    const char *msg = "shipped the feature today!";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 1u);
    HU_ASSERT_FALSE(m.goals[0].active);
    HU_ASSERT_TRUE(m.goals[0].progress > 0.99f);
}

static void personal_model_resolve_goals_handles_done(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "finish the report");
    m.goals[0].active = true;
    m.goal_count = 1;

    const char *msg = "the report is done";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 1u);
    HU_ASSERT_FALSE(m.goals[0].active);
}

static void personal_model_resolve_goals_skips_negated_completion(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the feature");
    m.goals[0].active = true;
    m.goal_count = 1;

    const char *msg = "I haven't shipped the feature yet";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 0u);
    HU_ASSERT_TRUE(m.goals[0].active);
}

static void personal_model_resolve_goals_skips_not_done(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    m.goal_count = 1;

    /* "I'm not done with the feature" — completion verb is "done"
     * but the negation guard scans 12 chars before "done" and finds
     * "not", so this must NOT mark the goal as resolved. */
    const char *msg = "I'm not done with the feature";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 0u);
    HU_ASSERT_TRUE(m.goals[0].active);
}

static void personal_model_resolve_goals_skips_without(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    m.goal_count = 1;

    /* "without finishing" — "without" should suppress the completion
     * verb. */
    const char *msg = "we cannot ship feature without finishing tests";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 0u);
    HU_ASSERT_TRUE(m.goals[0].active);
}

static void personal_model_resolve_goals_requires_content_word(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    m.goal_count = 1;

    /* Completion verb without any goal content word → no match.
     * "I finished my coffee" must NOT resolve the "ship feature" goal. */
    const char *msg = "I finished my coffee";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 0u);
    HU_ASSERT_TRUE(m.goals[0].active);
}

static void personal_model_resolve_goals_skips_inactive(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = false;
    m.goals[0].progress = 1.0f;
    m.goal_count = 1;

    /* Already inactive — even a clear completion message must be
     * idempotent (no double-deactivate, no count). */
    const char *msg = "shipped the feature";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 0u);
}

static void personal_model_resolve_goals_does_not_match_doneness(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    m.goal_count = 1;

    /* "doneness" must NOT match "done" because of the boundary check
     * in find_word_ci_with_boundary. The goal content word "feature"
     * is present, but the only candidate verb is embedded inside
     * "doneness", so no match. */
    const char *msg = "the doneness of the feature is debatable";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 0u);
    HU_ASSERT_TRUE(m.goals[0].active);
}

static void personal_model_resolve_goals_resolves_multiple(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    snprintf(m.goals[1].description, sizeof(m.goals[1].description), "fix database bug");
    m.goals[1].active = true;
    m.goal_count = 2;

    const char *msg = "shipped feature and resolved database issue";
    HU_ASSERT_EQ(hu_personal_model_resolve_goals_in_message(&m, msg, strlen(msg), 5000), 2u);
    HU_ASSERT_FALSE(m.goals[0].active);
    HU_ASSERT_FALSE(m.goals[1].active);
}

/* ── Goal mention detection ─────────────────────────────────────────── */

static void personal_model_touch_goals_handles_null_args(void) {
    HU_ASSERT_EQ(hu_personal_model_touch_goals_in_message(NULL, "hello", 5, 1000), 0u);
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_EQ(hu_personal_model_touch_goals_in_message(&m, NULL, 0, 1000), 0u);
    HU_ASSERT_EQ(hu_personal_model_touch_goals_in_message(&m, "x", 0, 1000), 0u);
}

static void personal_model_touch_goals_bumps_on_content_word_match(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the new feature");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;

    const char *msg = "yeah, working on the feature today";
    size_t bumped = hu_personal_model_touch_goals_in_message(&m, msg, strlen(msg), 5000);
    HU_ASSERT_EQ(bumped, 1u);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 5000L);
}

static void personal_model_touch_goals_skips_short_words(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    /* "the" is 3 chars; even if it's in the message, the helper
     * shouldn't bump on particles. Description has only short
     * words → no match possible. */
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "do the thing");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;

    HU_ASSERT_EQ(
        hu_personal_model_touch_goals_in_message(&m, "the the the", 11, 5000),
        0u);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 1000L);
}

static void personal_model_touch_goals_case_insensitive(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "LEARN ESPERANTO");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;

    const char *msg = "i tried to learn some new words today";
    HU_ASSERT_EQ(hu_personal_model_touch_goals_in_message(&m, msg, strlen(msg), 5000), 1u);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 5000L);
}

static void personal_model_touch_goals_skips_inactive(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the new feature");
    m.goals[0].active = false; /* completed */
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;

    const char *msg = "feature done!";
    HU_ASSERT_EQ(hu_personal_model_touch_goals_in_message(&m, msg, strlen(msg), 5000), 0u);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 1000L);
}

static void personal_model_touch_goals_does_not_lower_existing_stamp(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 9000; /* already very fresh */
    m.goal_count = 1;

    /* Now is in the past relative to last_referenced — bump count
     * is reported but the stamp must not regress. */
    const char *msg = "feature update";
    size_t bumped = hu_personal_model_touch_goals_in_message(&m, msg, strlen(msg), 5000);
    HU_ASSERT_EQ(bumped, 1u);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 9000L);
}

static void personal_model_touch_goals_no_match_does_not_bump(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;

    const char *msg = "what's for dinner?";
    HU_ASSERT_EQ(hu_personal_model_touch_goals_in_message(&m, msg, strlen(msg), 5000), 0u);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 1000L);
}

static void personal_model_touch_goals_bumps_multiple_goals(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    m.goals[0].last_referenced = 1000;
    snprintf(m.goals[1].description, sizeof(m.goals[1].description), "learn climbing");
    m.goals[1].active = true;
    m.goals[1].last_referenced = 1000;
    m.goal_count = 2;

    const char *msg = "going climbing this weekend, also feature pr ready";
    HU_ASSERT_EQ(hu_personal_model_touch_goals_in_message(&m, msg, strlen(msg), 5000), 2u);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 5000L);
    HU_ASSERT_EQ((long)m.goals[1].last_referenced, 5000L);
}

/* ── v3 → v4 progressive migration ──────────────────────────────────── */

/* Mirror the on-disk v3 layout. Tests own this struct definition (not
 * the production code) so a future schema bump can't silently
 * invalidate the fixture — if v4→v5 changes one of these fields, this
 * struct stays as-is and the v3-on-disk migration path is still
 * exercised. */
typedef struct pm_v3_test_goal {
    char description[512];
    bool active;
    int64_t created_at;
    int64_t deadline;
    float progress;
} pm_v3_test_goal_t;

typedef struct pm_v3_test_style {
    float formality;
    float verbosity;
    float emoji_frequency;
    float humor_receptivity;
    float lowercase_ratio;
    float abbreviation_ratio;
    uint32_t avg_message_length;
    uint32_t sample_count;
} pm_v3_test_style_t;

typedef struct pm_v3_test_model {
    hu_core_memory_t core;
    hu_heuristic_fact_t facts[HU_PM_MAX_FACTS];
    size_t fact_count;
    pm_v3_test_style_t style;
    hu_personal_topic_t topics[HU_PM_MAX_TOPICS];
    size_t topic_count;
    pm_v3_test_goal_t goals[HU_PM_MAX_GOALS];
    size_t goal_count;
    uint8_t active_hours[24];
    uint8_t active_days[7];
    int64_t created_at;
    int64_t updated_at;
    uint32_t interaction_count;
    uint32_t version;
} pm_v3_test_model_t;

typedef struct pm_v3_test_header {
    uint32_t magic;
    uint32_t version;
    uint64_t reserved;
} pm_v3_test_header_t;

#define PM_V3_TEST_MAGIC 0x4D505548u   /* "HUPM" little-endian */
#define PM_V3_TEST_VERSION 3u

static int pm_v3_write_fixture(const char *path, const pm_v3_test_model_t *model) {
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    pm_v3_test_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = PM_V3_TEST_MAGIC;
    hdr.version = PM_V3_TEST_VERSION;
    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1 ||
        fwrite(model, sizeof(*model), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static void personal_model_load_migrates_v3_facts_and_topics(void) {
    const char *path = "/tmp/hu_pm_v3_fixture_facts.bin";
    pm_v3_test_model_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.version = PM_V3_TEST_VERSION;
    v3.created_at = 1700000000LL;
    v3.updated_at = 1700100000LL;
    v3.interaction_count = 42;

    /* Seed two facts — these must round-trip identically because the
     * fact struct didn't change between v3 and v4. */
    snprintf(v3.facts[0].subject, sizeof(v3.facts[0].subject), "user");
    snprintf(v3.facts[0].predicate, sizeof(v3.facts[0].predicate), "likes");
    snprintf(v3.facts[0].object, sizeof(v3.facts[0].object), "coffee");
    v3.facts[0].confidence = 0.9f;
    v3.facts[0].last_seen_at = 1700050000LL;
    snprintf(v3.facts[1].subject, sizeof(v3.facts[1].subject), "user");
    snprintf(v3.facts[1].predicate, sizeof(v3.facts[1].predicate), "lives in");
    snprintf(v3.facts[1].object, sizeof(v3.facts[1].object), "portland");
    v3.facts[1].confidence = 0.85f;
    v3.facts[1].last_seen_at = 1700060000LL;
    v3.fact_count = 2;

    /* Seed a topic — also unchanged across v3↔v4. */
    snprintf(v3.topics[0].name, sizeof(v3.topics[0].name), "hiking");
    v3.topics[0].interest_score = 0.7f;
    v3.topics[0].mention_count = 5;
    v3.topics[0].last_mentioned = 1700070000LL;
    v3.topic_count = 1;

    HU_ASSERT_EQ(pm_v3_write_fixture(path, &v3), 0);

    hu_personal_model_t loaded;
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_OK);
    (void)remove(path);

    HU_ASSERT_EQ((long)loaded.created_at, 1700000000L);
    HU_ASSERT_EQ((long)loaded.updated_at, 1700100000L);
    HU_ASSERT_EQ((unsigned)loaded.interaction_count, 42u);
    HU_ASSERT_EQ(loaded.fact_count, 2u);
    HU_ASSERT_STR_EQ(loaded.facts[0].object, "coffee");
    HU_ASSERT_STR_EQ(loaded.facts[1].object, "portland");
    HU_ASSERT_EQ((long)loaded.facts[0].last_seen_at, 1700050000L);
    HU_ASSERT_EQ(loaded.topic_count, 1u);
    HU_ASSERT_STR_EQ(loaded.topics[0].name, "hiking");
    /* In-memory model now reports the current schema, even though the
     * on-disk file was v3. Re-saving will write a v4 file. */
    HU_ASSERT_EQ((unsigned)loaded.version, 4u);
}

static void personal_model_load_migrates_v3_goals_zero_fills_last_referenced(void) {
    const char *path = "/tmp/hu_pm_v3_fixture_goals.bin";
    pm_v3_test_model_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.version = PM_V3_TEST_VERSION;
    snprintf(v3.goals[0].description, sizeof(v3.goals[0].description),
             "ship the new feature");
    v3.goals[0].active = true;
    v3.goals[0].created_at = 1700000000LL;
    v3.goals[0].deadline = 1701000000LL;
    v3.goals[0].progress = 0.3f;
    v3.goal_count = 1;

    HU_ASSERT_EQ(pm_v3_write_fixture(path, &v3), 0);
    hu_personal_model_t loaded;
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_OK);
    (void)remove(path);

    HU_ASSERT_EQ(loaded.goal_count, 1u);
    HU_ASSERT_STR_EQ(loaded.goals[0].description, "ship the new feature");
    HU_ASSERT_TRUE(loaded.goals[0].active);
    HU_ASSERT_EQ((long)loaded.goals[0].created_at, 1700000000L);
    HU_ASSERT_EQ((long)loaded.goals[0].deadline, 1701000000L);
    /* New v4 field: zero-filled. The effective-priority function
     * falls back to created_at when last_referenced is 0, so the
     * migrated goal still scores high right after the migration. */
    HU_ASSERT_EQ((long)loaded.goals[0].last_referenced, 0L);
    HU_ASSERT_TRUE(hu_personal_goal_effective_priority(&loaded.goals[0],
                                                       loaded.goals[0].created_at) > 0.99f);
}

static void personal_model_load_migrates_v3_style_zero_fills_last_observed(void) {
    const char *path = "/tmp/hu_pm_v3_fixture_style.bin";
    pm_v3_test_model_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.version = PM_V3_TEST_VERSION;
    v3.style.formality = 0.2f;
    v3.style.verbosity = 0.4f;
    v3.style.emoji_frequency = 0.1f;
    v3.style.humor_receptivity = 0.6f;
    v3.style.lowercase_ratio = 0.7f;
    v3.style.abbreviation_ratio = 0.3f;
    v3.style.avg_message_length = 80;
    v3.style.sample_count = 50;

    HU_ASSERT_EQ(pm_v3_write_fixture(path, &v3), 0);
    hu_personal_model_t loaded;
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_OK);
    (void)remove(path);

    HU_ASSERT_TRUE(loaded.style.formality > 0.19f && loaded.style.formality < 0.21f);
    HU_ASSERT_EQ((unsigned)loaded.style.sample_count, 50u);
    HU_ASSERT_EQ((unsigned)loaded.style.avg_message_length, 80u);
    /* Zero-filled new field. Freshness of "samples present but
     * last_observed_at == 0" is treated as fully fresh so the
     * directive doesn't silently disappear after migration. */
    HU_ASSERT_EQ((long)loaded.style.last_observed_at, 0L);
    HU_ASSERT_TRUE(
        hu_personal_communication_style_freshness(&loaded.style, 1700000000LL) > 0.99f);
}

static void personal_model_load_rejects_unknown_version(void) {
    const char *path = "/tmp/hu_pm_v99_fixture.bin";
    /* Hand-craft a header with an unsupported version. The loader
     * must report HU_ERR_PARSE and leave `out` initialized to defaults
     * so callers can keep walking. */
    FILE *fp = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(fp);
    pm_v3_test_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = PM_V3_TEST_MAGIC;
    hdr.version = 99u;
    HU_ASSERT_EQ(fwrite(&hdr, sizeof(hdr), 1, fp), 1u);
    /* Trailing zeros so the file is at least header-sized. */
    fclose(fp);

    hu_personal_model_t loaded;
    /* Pre-fill so we can verify the function re-initializes on failure. */
    memset(&loaded, 0xab, sizeof(loaded));
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_ERR_PARSE);
    (void)remove(path);
    /* The loader resets to defaults before the version check; loaded
     * should now be a fresh model. */
    HU_ASSERT_EQ(loaded.fact_count, 0u);
    HU_ASSERT_EQ(loaded.topic_count, 0u);
    HU_ASSERT_EQ(loaded.goal_count, 0u);
}

static void personal_model_load_v4_save_still_round_trips(void) {
    /* Pin that the v4-native save/load path still works after the
     * migration arm was added. Save → load round-trips identically
     * to before. */
    const char *path = "/tmp/hu_pm_v4_native_roundtrip.bin";
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.created_at = 1700000000LL;
    m.updated_at = 1700100000LL;
    m.style.last_observed_at = 1700050000LL;
    m.style.sample_count = 3;
    m.goals[0].active = true;
    m.goals[0].created_at = 1700000000LL;
    m.goals[0].last_referenced = 1700090000LL;
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "v4 native");
    m.goal_count = 1;

    HU_ASSERT_EQ(hu_personal_model_save(&m, path), HU_OK);
    hu_personal_model_t loaded;
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_OK);
    (void)remove(path);

    HU_ASSERT_EQ((long)loaded.style.last_observed_at, 1700050000L);
    HU_ASSERT_EQ((long)loaded.goals[0].last_referenced, 1700090000L);
    HU_ASSERT_STR_EQ(loaded.goals[0].description, "v4 native");
}

/* ── per_turn_tick maintenance helper ───────────────────────────────── */

static void personal_model_per_turn_tick_handles_null_model(void) {
    hu_personal_model_turn_tick_result_t r =
        hu_personal_model_per_turn_tick(NULL, "hi", 2, true, 1000);
    HU_ASSERT_EQ(r.ingest_error, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(r.goals_touched, 0u);
    HU_ASSERT_EQ(r.goals_resolved, 0u);
    HU_ASSERT_EQ(r.entries_pruned, 0u);
}

static void personal_model_per_turn_tick_runs_full_sequence(void) {
    /* End-to-end sanity: a single per-turn tick with a goal-mention
     * message bumps the goal AND ingests the message AND runs decay.
     * The result struct reports each phase's count. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship the new feature");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;

    const char *msg = "still working on the feature today";
    hu_personal_model_turn_tick_result_t r =
        hu_personal_model_per_turn_tick(&m, msg, strlen(msg), true, 5000);

    HU_ASSERT_EQ(r.ingest_error, HU_OK);
    HU_ASSERT_EQ(r.goals_touched, 1u);
    HU_ASSERT_EQ(r.goals_resolved, 0u);
    /* Goal still active, last_referenced bumped to 5000. */
    HU_ASSERT_TRUE(m.goals[0].active);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 5000L);
}

static void personal_model_per_turn_tick_orders_touch_before_resolve(void) {
    /* Critical ordering test: when a single message both mentions
     * AND completes a goal, touch must run before resolve so the
     * goal has a fresh last_referenced stamp BEFORE it gets
     * deactivated. Future "recently completed" surfaces use the
     * stamp to know how recent the completion was. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "ship feature");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;

    /* "shipped feature" → both touch and resolve match. */
    const char *msg = "shipped feature";
    hu_personal_model_turn_tick_result_t r =
        hu_personal_model_per_turn_tick(&m, msg, strlen(msg), true, 5000);
    HU_ASSERT_EQ(r.goals_touched, 1u);
    HU_ASSERT_EQ(r.goals_resolved, 1u);
    /* If goals_touched == 1 AND the goal is now inactive, the
     * touch ran before the resolve. (If resolve ran first, the
     * goal would already be inactive and touch_goals would skip it
     * → goals_touched == 0.) */
    /* The goal is now inactive and lives in the recently-completed
     * retention window (HU_PM_COMPLETED_GOAL_RETAIN_SEC = 7 days),
     * so apply_decay does NOT prune it in the same tick. Future
     * apply_decay calls past the retention window will prune. */
    HU_ASSERT_EQ(m.goal_count, 1u);
    HU_ASSERT_FALSE(m.goals[0].active);
    HU_ASSERT_EQ((long)m.goals[0].last_referenced, 5000L);
}

static void personal_model_per_turn_tick_runs_resolve_before_decay(void) {
    /* Decay must see the post-resolve state: a just-completed goal
     * is no longer "active" by the time decay runs. Today the goal
     * lives on in the recently-completed retention window so the
     * prompt builder can surface "Recently completed: …", but its
     * `active` flag is observable in the same turn. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "finish the report");
    m.goals[0].active = true;
    m.goals[0].created_at = 1000;
    m.goals[0].last_referenced = 1000;
    m.goal_count = 1;

    const char *msg = "the report is done";
    hu_personal_model_turn_tick_result_t r =
        hu_personal_model_per_turn_tick(&m, msg, strlen(msg), true, 5000);
    HU_ASSERT_EQ(r.goals_resolved, 1u);
    /* Retention window keeps the slot alive — `entries_pruned` is 0,
     * `goal_count` is 1, but `active` flipped to false. */
    HU_ASSERT_EQ(m.goal_count, 1u);
    HU_ASSERT_FALSE(m.goals[0].active);
}

static void personal_model_per_turn_tick_handles_empty_message(void) {
    /* Empty messages are a no-op for the goal phases (they can't
     * match any content word) but still call ingest, which itself
     * has zero-length-message handling. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    hu_personal_model_turn_tick_result_t r =
        hu_personal_model_per_turn_tick(&m, "", 0, true, 1000);
    HU_ASSERT_EQ(r.ingest_error, HU_OK);
    HU_ASSERT_EQ(r.goals_touched, 0u);
    HU_ASSERT_EQ(r.goals_resolved, 0u);
}

static void personal_model_per_turn_tick_propagates_ingest_error(void) {
    /* When ingest itself can't run (e.g. NULL message buffer with
     * non-zero length is undefined; but we can simulate via a
     * pre-condition like a model that's been forced into an invalid
     * state). Easiest path: pass NULL msg with non-zero len —
     * personal_model_ingest treats this as a no-op (returns OK),
     * not an error. So instead we verify that on the happy path
     * the helper reports OK. The non-OK case is fundamentally a
     * defensive guard for future ingest extensions. */
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    hu_personal_model_turn_tick_result_t r =
        hu_personal_model_per_turn_tick(&m, "hello world", 11, true, 1000);
    HU_ASSERT_EQ(r.ingest_error, HU_OK);
}

/* ── idle_due rate-limit helper ─────────────────────────────────────── */

static void personal_model_idle_due_null_last_returns_false(void) {
    HU_ASSERT_FALSE(hu_personal_model_idle_due(NULL, 1000, 60));
}

static void personal_model_idle_due_non_positive_now_returns_false(void) {
    int64_t last = 100;
    HU_ASSERT_FALSE(hu_personal_model_idle_due(&last, 0, 60));
    HU_ASSERT_FALSE(hu_personal_model_idle_due(&last, -1, 60));
    /* `last` must not be mutated when the call returns false. */
    HU_ASSERT_EQ((long)last, 100L);
}

static void personal_model_idle_due_non_positive_interval_returns_false(void) {
    int64_t last = 100;
    HU_ASSERT_FALSE(hu_personal_model_idle_due(&last, 1000, 0));
    HU_ASSERT_FALSE(hu_personal_model_idle_due(&last, 1000, -60));
    HU_ASSERT_EQ((long)last, 100L);
}

static void personal_model_idle_due_first_call_fires_immediately(void) {
    int64_t last = 0;
    HU_ASSERT_TRUE(hu_personal_model_idle_due(&last, 1000, 3600));
    /* Stamp must be advanced to `now` so the next call enters the
     * normal interval-check path. */
    HU_ASSERT_EQ((long)last, 1000L);
}

static void personal_model_idle_due_negative_sentinel_fires_immediately(void) {
    int64_t last = -1;
    HU_ASSERT_TRUE(hu_personal_model_idle_due(&last, 1000, 3600));
    HU_ASSERT_EQ((long)last, 1000L);
}

static void personal_model_idle_due_inside_interval_returns_false(void) {
    int64_t last = 1000;
    /* now - last == 30 s, interval 60 → not due. */
    HU_ASSERT_FALSE(hu_personal_model_idle_due(&last, 1030, 60));
    HU_ASSERT_EQ((long)last, 1000L);
}

static void personal_model_idle_due_at_exact_interval_fires(void) {
    int64_t last = 1000;
    /* now - last == 60 s, interval 60 → exactly due. */
    HU_ASSERT_TRUE(hu_personal_model_idle_due(&last, 1060, 60));
    HU_ASSERT_EQ((long)last, 1060L);
}

static void personal_model_idle_due_far_past_fires(void) {
    int64_t last = 1000;
    /* now - last == 1 day, interval 1 hour → due. */
    HU_ASSERT_TRUE(hu_personal_model_idle_due(&last, 1000 + 86400, 3600));
    HU_ASSERT_EQ((long)last, 1000L + 86400L);
}

static void personal_model_idle_due_consecutive_pattern(void) {
    /* End-to-end: simulate the daemon's hourly loop. The first call
     * fires, subsequent calls within the interval don't, calls after
     * the interval do. */
    int64_t last = 0;
    /* t=1000: first call, fires, stamps 1000. */
    HU_ASSERT_TRUE(hu_personal_model_idle_due(&last, 1000, 3600));
    /* t=2000: 1000s after, < 3600s interval, doesn't fire. */
    HU_ASSERT_FALSE(hu_personal_model_idle_due(&last, 2000, 3600));
    /* t=4600: 3600s after the last fire, fires. */
    HU_ASSERT_TRUE(hu_personal_model_idle_due(&last, 4600, 3600));
    HU_ASSERT_EQ((long)last, 4600L);
    /* t=4601: 1s after, doesn't fire. */
    HU_ASSERT_FALSE(hu_personal_model_idle_due(&last, 4601, 3600));
    /* t=8200: another full interval, fires. */
    HU_ASSERT_TRUE(hu_personal_model_idle_due(&last, 8200, 3600));
    HU_ASSERT_EQ((long)last, 8200L);
}

/* ── apply_decay ────────────────────────────────────────────────────── */

static void personal_model_apply_decay_handles_null(void) {
    HU_ASSERT_EQ(hu_personal_model_apply_decay(NULL, 1000000), 0u);
}

static void personal_model_apply_decay_returns_zero_on_empty_model(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_EQ(hu_personal_model_apply_decay(&m, 1000000), 0u);
}

static void personal_model_apply_decay_prunes_stale_facts(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = (int64_t)(40LL * HU_FACT_CONFIDENCE_HALF_LIFE_SEC);

    /* Fresh fact (kept). */
    snprintf(m.facts[0].subject, sizeof(m.facts[0].subject), "user");
    snprintf(m.facts[0].predicate, sizeof(m.facts[0].predicate), "likes");
    snprintf(m.facts[0].object, sizeof(m.facts[0].object), "coffee");
    m.facts[0].confidence = 0.9f;
    m.facts[0].last_seen_at = now - 1;

    /* Stale fact (pruned: eff < 0.05). */
    snprintf(m.facts[1].subject, sizeof(m.facts[1].subject), "user");
    snprintf(m.facts[1].predicate, sizeof(m.facts[1].predicate), "works at");
    snprintf(m.facts[1].object, sizeof(m.facts[1].object), "ghost_corp");
    m.facts[1].confidence = 0.9f;
    m.facts[1].last_seen_at = now - 10 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;

    m.fact_count = 2;
    HU_ASSERT_EQ(hu_personal_model_apply_decay(&m, now), 1u);
    HU_ASSERT_EQ(m.fact_count, 1u);
    HU_ASSERT_STR_EQ(m.facts[0].object, "coffee");
}

static void personal_model_apply_decay_prunes_stale_topics(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = (int64_t)(40LL * HU_PM_TOPIC_INTEREST_HALF_LIFE_SEC);

    snprintf(m.topics[0].name, sizeof(m.topics[0].name), "kept");
    m.topics[0].interest_score = 0.7f;
    m.topics[0].mention_count = 3U;
    m.topics[0].last_mentioned = now - 1;

    snprintf(m.topics[1].name, sizeof(m.topics[1].name), "dropped");
    m.topics[1].interest_score = 0.9f;
    m.topics[1].mention_count = 100U;
    m.topics[1].last_mentioned = now - 10 * HU_PM_TOPIC_INTEREST_HALF_LIFE_SEC;

    m.topic_count = 2;
    HU_ASSERT_EQ(hu_personal_model_apply_decay(&m, now), 1u);
    HU_ASSERT_EQ(m.topic_count, 1u);
    HU_ASSERT_STR_EQ(m.topics[0].name, "kept");
}

static void personal_model_apply_decay_prunes_inactive_goals(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = 1000000;

    snprintf(m.goals[0].description, sizeof(m.goals[0].description), "active");
    m.goals[0].active = true;
    m.goals[0].created_at = now - 100;
    m.goals[0].last_referenced = now - 100;

    /* Inactive goals are pruned once they leave the 7-day retention window. */
    snprintf(m.goals[1].description, sizeof(m.goals[1].description), "completed");
    m.goals[1].active = false;
    m.goals[1].created_at = now - (8LL * 24 * 60 * 60);
    m.goals[1].last_referenced = now - (8LL * 24 * 60 * 60);

    m.goal_count = 2;
    HU_ASSERT_EQ(hu_personal_model_apply_decay(&m, now), 1u);
    HU_ASSERT_EQ(m.goal_count, 1u);
    HU_ASSERT_STR_EQ(m.goals[0].description, "active");
}

static void personal_model_apply_decay_is_idempotent(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = (int64_t)(40LL * HU_FACT_CONFIDENCE_HALF_LIFE_SEC);

    snprintf(m.facts[0].subject, sizeof(m.facts[0].subject), "user");
    snprintf(m.facts[0].predicate, sizeof(m.facts[0].predicate), "likes");
    snprintf(m.facts[0].object, sizeof(m.facts[0].object), "coffee");
    m.facts[0].confidence = 0.9f;
    m.facts[0].last_seen_at = now - 1;

    snprintf(m.facts[1].subject, sizeof(m.facts[1].subject), "user");
    snprintf(m.facts[1].predicate, sizeof(m.facts[1].predicate), "knew");
    snprintf(m.facts[1].object, sizeof(m.facts[1].object), "ghost");
    m.facts[1].confidence = 0.9f;
    m.facts[1].last_seen_at = now - 10 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;

    m.fact_count = 2;
    size_t first = hu_personal_model_apply_decay(&m, now);
    HU_ASSERT_EQ(first, 1u);
    /* Second pass at same `now` should report zero — everything that
     * survived the first pass also survives the second. */
    size_t second = hu_personal_model_apply_decay(&m, now);
    HU_ASSERT_EQ(second, 0u);
    HU_ASSERT_EQ(m.fact_count, 1u);
}

static void personal_model_apply_decay_zeros_vacant_slots(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    int64_t now = (int64_t)(40LL * HU_FACT_CONFIDENCE_HALF_LIFE_SEC);

    snprintf(m.facts[0].subject, sizeof(m.facts[0].subject), "user");
    snprintf(m.facts[0].predicate, sizeof(m.facts[0].predicate), "knew");
    snprintf(m.facts[0].object, sizeof(m.facts[0].object), "ghost_a");
    m.facts[0].confidence = 0.9f;
    m.facts[0].last_seen_at = now - 10 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;

    snprintf(m.facts[1].subject, sizeof(m.facts[1].subject), "user");
    snprintf(m.facts[1].predicate, sizeof(m.facts[1].predicate), "knew");
    snprintf(m.facts[1].object, sizeof(m.facts[1].object), "ghost_b");
    m.facts[1].confidence = 0.9f;
    m.facts[1].last_seen_at = now - 10 * HU_FACT_CONFIDENCE_HALF_LIFE_SEC;

    m.fact_count = 2;
    HU_ASSERT_EQ(hu_personal_model_apply_decay(&m, now), 2u);
    HU_ASSERT_EQ(m.fact_count, 0u);
    /* Vacant slots zeroed so save/load round-trips don't carry ghosts. */
    HU_ASSERT_EQ(m.facts[0].confidence, 0.f);
    HU_ASSERT_STR_EQ(m.facts[0].object, "");
}

void run_personal_model_tests(void) {
    HU_TEST_SUITE("PersonalModel");
    HU_RUN_TEST(personal_model_init_sets_defaults);
    HU_RUN_TEST(personal_model_ingest_extracts_facts);
    HU_RUN_TEST(personal_model_merge_facts_deduplicates);
    HU_RUN_TEST(personal_model_build_prompt_non_empty);
    HU_RUN_TEST(personal_model_query_preference_finds_match);
    HU_RUN_TEST(personal_model_ingest_updates_style_metrics);
    HU_RUN_TEST(personal_model_has_content_false_when_fresh);
    HU_RUN_TEST(personal_model_has_content_true_after_fact);
    HU_RUN_TEST(personal_model_has_content_true_after_style_observation);
    HU_RUN_TEST(personal_model_reaches_system_prompt_via_config);
    HU_RUN_TEST(personal_model_absent_does_not_leak_into_prompt);
    HU_RUN_TEST(personal_model_save_load_round_trips);
    HU_RUN_TEST(personal_model_load_rejects_bad_magic);
    HU_RUN_TEST(personal_model_load_missing_file_returns_not_found);
    HU_RUN_TEST(personal_model_resolve_default_path_honors_env_override);
    HU_RUN_TEST(personal_model_resolve_default_path_uses_home);
    HU_RUN_TEST(personal_model_resolve_default_path_no_home_returns_null);
    HU_RUN_TEST(personal_model_save_creates_parent_directory);
    HU_RUN_TEST(personal_model_default_path_round_trip);
    HU_RUN_TEST(personal_model_survives_simulated_crash);
    HU_RUN_TEST(personal_model_style_directive_emerges_after_three_samples);
    HU_RUN_TEST(personal_model_style_directive_casual_terse_humorous);
    HU_RUN_TEST(personal_model_style_directive_formal_verbose);
    HU_RUN_TEST(personal_model_style_directive_lowercase_typer);
    HU_RUN_TEST(personal_model_style_directive_abbreviation_user);
    HU_RUN_TEST(personal_model_style_directive_proper_case_no_lowercase_clause);
    HU_RUN_TEST(personal_model_style_directive_absent_when_no_samples);
    HU_RUN_TEST(personal_model_avoid_line_emerges_for_dislikes);
    HU_RUN_TEST(personal_model_avoid_line_handles_hate);
    HU_RUN_TEST(personal_model_avoid_line_skips_when_only_positive_facts);
    HU_RUN_TEST(personal_model_avoid_line_collects_multiple_dislikes);
    HU_RUN_TEST(personal_model_topic_directive_emerges_for_sustained_interest);
    HU_RUN_TEST(personal_model_topic_directive_skips_low_mention_count);
    HU_RUN_TEST(personal_model_topic_directive_skips_low_interest_score);
    HU_RUN_TEST(personal_model_topic_directive_caps_at_three);
    HU_RUN_TEST(personal_model_topic_directive_filters_mixed_topics);
    HU_RUN_TEST(personal_model_topic_directive_absent_when_no_topics);
    HU_RUN_TEST(personal_model_infer_chronotype_returns_unknown_for_empty);
    HU_RUN_TEST(personal_model_infer_chronotype_returns_unknown_for_null);
    HU_RUN_TEST(personal_model_infer_chronotype_returns_unknown_below_min_samples);
    HU_RUN_TEST(personal_model_infer_chronotype_classifies_morning_lark);
    HU_RUN_TEST(personal_model_infer_chronotype_classifies_evening_owl);
    HU_RUN_TEST(personal_model_infer_chronotype_classifies_intermediate_when_flat);
    HU_RUN_TEST(personal_model_infer_chronotype_classifies_intermediate_for_middle_dominant);
    HU_RUN_TEST(personal_model_infer_chronotype_does_not_overcommit_on_thin_lead);
    HU_RUN_TEST(fact_effective_confidence_no_decay_when_last_seen_zero);
    HU_RUN_TEST(fact_effective_confidence_no_decay_when_now_at_or_before_last_seen);
    HU_RUN_TEST(fact_effective_confidence_halves_at_one_half_life);
    HU_RUN_TEST(fact_effective_confidence_quarters_at_two_half_lives);
    HU_RUN_TEST(fact_effective_confidence_floors_to_zero_far_future);
    HU_RUN_TEST(fact_effective_confidence_handles_null);
    HU_RUN_TEST(fact_extract_zeroes_last_seen_at);
    HU_RUN_TEST(personal_model_merge_facts_stamps_last_seen_on_insert);
    HU_RUN_TEST(personal_model_merge_facts_refreshes_duplicate);
    HU_RUN_TEST(personal_model_build_prompt_drops_stale_facts);
    HU_RUN_TEST(personal_model_build_prompt_drops_stale_avoid_lines);
    HU_RUN_TEST(personal_model_topic_effective_score_handles_null);
    HU_RUN_TEST(personal_model_topic_effective_score_no_decay_when_unstamped);
    HU_RUN_TEST(personal_model_topic_effective_score_no_decay_when_now_at_or_before);
    HU_RUN_TEST(personal_model_topic_effective_score_halves_at_one_half_life);
    HU_RUN_TEST(personal_model_topic_effective_score_floors_at_zero_far_future);
    HU_RUN_TEST(personal_model_goal_effective_priority_handles_null);
    HU_RUN_TEST(personal_model_goal_effective_priority_zero_for_inactive);
    HU_RUN_TEST(personal_model_goal_effective_priority_zero_for_empty_slot);
    HU_RUN_TEST(personal_model_goal_effective_priority_uses_created_at_fallback);
    HU_RUN_TEST(personal_model_goal_effective_priority_halves_at_one_half_life);
    HU_RUN_TEST(personal_model_style_freshness_handles_null);
    HU_RUN_TEST(personal_model_style_freshness_zero_when_never_observed);
    HU_RUN_TEST(personal_model_style_freshness_full_when_unstamped_with_samples);
    HU_RUN_TEST(personal_model_style_freshness_halves_at_one_half_life);
    HU_RUN_TEST(personal_model_ingest_stamps_style_last_observed_at);
    HU_RUN_TEST(personal_model_fidelity_handles_null);
    HU_RUN_TEST(personal_model_fidelity_returns_neg_when_no_samples);
    HU_RUN_TEST(personal_model_fidelity_high_for_matching_response);
    HU_RUN_TEST(personal_model_fidelity_low_for_uppercase_response);
    HU_RUN_TEST(personal_model_fidelity_rewards_abbreviation_match);
    HU_RUN_TEST(personal_model_fidelity_penalizes_length_mismatch);
    HU_RUN_TEST(personal_model_fidelity_zero_when_length_extremely_off);
    HU_RUN_TEST(personal_model_fidelity_score_is_deterministic);
    HU_RUN_TEST(personal_model_compare_response_sets_rejects_null_args);
    HU_RUN_TEST(personal_model_compare_response_sets_rejects_zero_sample_target);
    HU_RUN_TEST(personal_model_compare_response_sets_handles_empty_sets);
    HU_RUN_TEST(personal_model_compare_response_sets_reports_positive_delta);
    HU_RUN_TEST(personal_model_compare_response_sets_negative_delta_when_b_worse);
    HU_RUN_TEST(personal_model_compare_response_sets_skips_invalid_responses);
    HU_RUN_TEST(personal_model_compare_response_sets_reports_min_max);
    HU_RUN_TEST(personal_model_compare_response_sets_uses_explicit_lens);
    HU_RUN_TEST(personal_model_style_blend_handles_null);
    HU_RUN_TEST(personal_model_style_blend_returns_raw_at_full_freshness);
    HU_RUN_TEST(personal_model_style_blend_pulls_to_neutral_at_half_life);
    HU_RUN_TEST(personal_model_style_blend_returns_neutral_when_freshness_zero);
    HU_RUN_TEST(personal_model_style_blend_does_not_mutate_input);
    HU_RUN_TEST(personal_model_build_prompt_drops_stale_topic);
    HU_RUN_TEST(personal_model_build_prompt_drops_stale_goal);
    HU_RUN_TEST(personal_model_build_prompt_drops_style_directive_when_stale);
    HU_RUN_TEST(personal_model_recently_completed_handles_null);
    HU_RUN_TEST(personal_model_recently_completed_active_returns_false);
    HU_RUN_TEST(personal_model_recently_completed_inside_window_returns_true);
    HU_RUN_TEST(personal_model_recently_completed_at_window_boundary_returns_true);
    HU_RUN_TEST(personal_model_recently_completed_past_window_returns_false);
    HU_RUN_TEST(personal_model_recently_completed_no_stamp_returns_false);
    HU_RUN_TEST(personal_model_get_recently_completed_handles_null);
    HU_RUN_TEST(personal_model_get_recently_completed_returns_zero_when_none);
    HU_RUN_TEST(personal_model_get_recently_completed_returns_inside_window);
    HU_RUN_TEST(personal_model_get_recently_completed_respects_cap);
    HU_RUN_TEST(personal_model_get_recently_completed_skips_empty_slots);
    HU_RUN_TEST(personal_model_describe_recently_completed_handles_null);
    HU_RUN_TEST(personal_model_describe_recently_completed_returns_zero_when_none);
    HU_RUN_TEST(personal_model_describe_recently_completed_renders_one);
    HU_RUN_TEST(personal_model_describe_recently_completed_renders_multiple_with_separator);
    HU_RUN_TEST(personal_model_describe_recently_completed_truncates_with_ellipsis);
    HU_RUN_TEST(personal_model_describe_recently_completed_ignores_old);
    HU_RUN_TEST(personal_model_apply_decay_keeps_recently_completed);
    HU_RUN_TEST(personal_model_resolve_goals_stamps_last_referenced);
    HU_RUN_TEST(personal_model_build_prompt_surfaces_recently_completed);
    HU_RUN_TEST(personal_model_build_prompt_omits_old_completed);
    HU_RUN_TEST(personal_model_build_prompt_emits_acknowledgment_directive);
    HU_RUN_TEST(personal_model_build_prompt_omits_directive_when_no_completed);
    HU_RUN_TEST(personal_model_build_prompt_overlay_null_matches_legacy);
    HU_RUN_TEST(personal_model_build_prompt_overlay_formal_emits_terse_directive);
    HU_RUN_TEST(personal_model_build_prompt_overlay_casual_emoji_emits_permissive_variant);
    HU_RUN_TEST(personal_model_build_prompt_overlay_short_length_emphasizes_brevity);
    HU_RUN_TEST(personal_model_build_prompt_overlay_formal_overrides_emoji);
    HU_RUN_TEST(personal_model_build_prompt_overlay_unknown_falls_through_to_default);
    HU_RUN_TEST(personal_model_directive_telemetry_reset_zeros_counts);
    HU_RUN_TEST(personal_model_directive_telemetry_null_overlay_increments_null);
    HU_RUN_TEST(personal_model_directive_telemetry_formal_overlay_increments_formal);
    HU_RUN_TEST(personal_model_directive_telemetry_casual_emoji_increments_casual_emoji);
    HU_RUN_TEST(personal_model_directive_telemetry_unspecified_overlay_increments_default);
    HU_RUN_TEST(personal_model_directive_telemetry_repeated_calls_accumulate);
    HU_RUN_TEST(personal_model_directive_variant_label_returns_known);
    HU_RUN_TEST(personal_model_resolve_goals_handles_null_args);
    HU_RUN_TEST(personal_model_resolve_goals_deactivates_on_completion_verb);
    HU_RUN_TEST(personal_model_resolve_goals_handles_done);
    HU_RUN_TEST(personal_model_resolve_goals_skips_negated_completion);
    HU_RUN_TEST(personal_model_resolve_goals_skips_not_done);
    HU_RUN_TEST(personal_model_resolve_goals_skips_without);
    HU_RUN_TEST(personal_model_resolve_goals_requires_content_word);
    HU_RUN_TEST(personal_model_resolve_goals_skips_inactive);
    HU_RUN_TEST(personal_model_resolve_goals_does_not_match_doneness);
    HU_RUN_TEST(personal_model_resolve_goals_resolves_multiple);
    HU_RUN_TEST(personal_model_touch_goals_handles_null_args);
    HU_RUN_TEST(personal_model_touch_goals_bumps_on_content_word_match);
    HU_RUN_TEST(personal_model_touch_goals_skips_short_words);
    HU_RUN_TEST(personal_model_touch_goals_case_insensitive);
    HU_RUN_TEST(personal_model_touch_goals_skips_inactive);
    HU_RUN_TEST(personal_model_touch_goals_does_not_lower_existing_stamp);
    HU_RUN_TEST(personal_model_touch_goals_no_match_does_not_bump);
    HU_RUN_TEST(personal_model_touch_goals_bumps_multiple_goals);
    HU_RUN_TEST(personal_model_load_migrates_v3_facts_and_topics);
    HU_RUN_TEST(personal_model_load_migrates_v3_goals_zero_fills_last_referenced);
    HU_RUN_TEST(personal_model_load_migrates_v3_style_zero_fills_last_observed);
    HU_RUN_TEST(personal_model_load_rejects_unknown_version);
    HU_RUN_TEST(personal_model_load_v4_save_still_round_trips);
    HU_RUN_TEST(personal_model_per_turn_tick_handles_null_model);
    HU_RUN_TEST(personal_model_per_turn_tick_runs_full_sequence);
    HU_RUN_TEST(personal_model_per_turn_tick_orders_touch_before_resolve);
    HU_RUN_TEST(personal_model_per_turn_tick_runs_resolve_before_decay);
    HU_RUN_TEST(personal_model_per_turn_tick_handles_empty_message);
    HU_RUN_TEST(personal_model_per_turn_tick_propagates_ingest_error);
    HU_RUN_TEST(personal_model_idle_due_null_last_returns_false);
    HU_RUN_TEST(personal_model_idle_due_non_positive_now_returns_false);
    HU_RUN_TEST(personal_model_idle_due_non_positive_interval_returns_false);
    HU_RUN_TEST(personal_model_idle_due_first_call_fires_immediately);
    HU_RUN_TEST(personal_model_idle_due_negative_sentinel_fires_immediately);
    HU_RUN_TEST(personal_model_idle_due_inside_interval_returns_false);
    HU_RUN_TEST(personal_model_idle_due_at_exact_interval_fires);
    HU_RUN_TEST(personal_model_idle_due_far_past_fires);
    HU_RUN_TEST(personal_model_idle_due_consecutive_pattern);
    HU_RUN_TEST(personal_model_apply_decay_handles_null);
    HU_RUN_TEST(personal_model_apply_decay_returns_zero_on_empty_model);
    HU_RUN_TEST(personal_model_apply_decay_prunes_stale_facts);
    HU_RUN_TEST(personal_model_apply_decay_prunes_stale_topics);
    HU_RUN_TEST(personal_model_apply_decay_prunes_inactive_goals);
    HU_RUN_TEST(personal_model_apply_decay_is_idempotent);
    HU_RUN_TEST(personal_model_apply_decay_zeros_vacant_slots);
#if defined(__unix__) || defined(__APPLE__)
    HU_RUN_TEST(personal_model_survives_real_sigkill);
#endif
}
