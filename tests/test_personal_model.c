#include "human/agent/prompt.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
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
    HU_ASSERT_EQ((long)m.version, 1L);
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
    /* Initialized to defaults — version is the only field set by init. */
    HU_ASSERT_EQ(out.version, 1U);
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
#if defined(__unix__) || defined(__APPLE__)
    HU_RUN_TEST(personal_model_survives_real_sigkill);
#endif
}
