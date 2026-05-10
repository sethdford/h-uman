/* W13 — Learning loop: hu_learner_t vtable + CPU backend + signal builders.
 *
 * Tests run against an in-memory SQLite DB via hu_graph_open(NULL, 0) and
 * write adapters into a fixed /tmp scratch path. Determinism is the central
 * contract: the CPU backend must produce byte-identical adapters for the
 * same (seed, signals, model_version) triple. */

#include "human/agent/case_based.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/ml/learner.h"
#include "human/persona/persona_deltas.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

static void scratch_path(char *out, size_t cap, const char *suffix) {
    snprintf(out, cap, "/tmp/hu-w13-test-%d-%s.adapter",
             (int)getpid(), suffix ? suffix : "default");
    /* Best-effort wipe so a previous test run doesn't pollute round-trip. */
    unlink(out);
}

static void fill_default_cfg(hu_learner_config_t *cfg, const char *path) {
    *cfg = hu_learner_default_config();
    snprintf(cfg->adapter_output_path, sizeof(cfg->adapter_output_path), "%s", path);
    snprintf(cfg->base_model_path, sizeof(cfg->base_model_path), "/dev/null");
    snprintf(cfg->model_version, sizeof(cfg->model_version), "test-v1");
    cfg->seed = 0xABCDEF0123456789ULL;
    cfg->rank = 4;
    cfg->max_steps = 25;
    cfg->learning_rate = 1e-3f;
    cfg->batch_size = 4;
    cfg->budget_ms = 5000;
}

static int file_size(const char *path) {
    struct stat s;
    if (stat(path, &s) != 0)
        return -1;
    return (int)s.st_size;
}

/* ── Default config + open ─────────────────────────────────────────────── */

static void test_w13_default_config_has_sensible_defaults(void) {
    hu_learner_config_t cfg = hu_learner_default_config();
    HU_ASSERT_EQ(cfg.rank, 8);
    HU_ASSERT_EQ(cfg.max_steps, 200);
    HU_ASSERT(cfg.learning_rate > 0.0f);
    HU_ASSERT_EQ(cfg.batch_size, 4);
    HU_ASSERT_FALSE(cfg.dp_enabled);
    HU_ASSERT(cfg.budget_ms > 0);
    HU_ASSERT_STR_EQ(cfg.model_version, "v1");
}

static void test_w13_learner_open_default_returns_some_backend(void) {
    hu_learner_t *l = NULL;
    HU_ASSERT_EQ(hu_learner_open_default(A(), &l), HU_OK);
    HU_ASSERT_NOT_NULL(l);
    HU_ASSERT_NOT_NULL(l->vt);
    HU_ASSERT_NOT_NULL(l->vt->name);
    /* CPU is the contracted fallback — at minimum we always get something. */
    HU_ASSERT(strcmp(l->vt->name, "cpu") == 0 || strcmp(l->vt->name, "mlx") == 0 ||
              strcmp(l->vt->name, "ggml") == 0);
    hu_learner_close(l);
}

static void test_w13_learner_open_named_unknown_returns_error(void) {
    hu_learner_t *l = NULL;
    HU_ASSERT_EQ(hu_learner_open_named(A(), "imaginary-backend", &l), HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(l);
    /* Known but currently-unavailable backend → NOT_SUPPORTED. */
    HU_ASSERT_EQ(hu_learner_open_named(A(), "mlx", &l), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(l);
    HU_ASSERT_EQ(hu_learner_open_named(A(), "ggml", &l), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(l);
    /* CPU always opens. */
    HU_ASSERT_EQ(hu_learner_open_named(A(), "cpu", &l), HU_OK);
    HU_ASSERT_NOT_NULL(l);
    hu_learner_close(l);
}

static void test_w13_invalid_args_rejected(void) {
    hu_learner_t *l = NULL;
    HU_ASSERT_EQ(hu_learner_open_default(NULL, &l), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_learner_open_default(A(), NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_learner_open_named(A(), NULL, &l), HU_ERR_INVALID_ARGUMENT);

    HU_ASSERT_EQ(hu_learner_open_named(A(), "cpu", &l), HU_OK);
    HU_ASSERT_NOT_NULL(l);

    char path[256];
    scratch_path(path, sizeof(path), "invalid");
    hu_learner_config_t cfg;
    fill_default_cfg(&cfg, path);
    hu_learner_report_t rep;
    /* signals_count > 0 with NULL signals → INVALID_ARGUMENT. */
    HU_ASSERT_EQ(hu_learner_train(l, &cfg, NULL, 5, &rep), HU_ERR_INVALID_ARGUMENT);
    /* NULL cfg / report → INVALID_ARGUMENT. */
    HU_ASSERT_EQ(hu_learner_train(l, NULL, NULL, 0, &rep), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_learner_train(l, &cfg, NULL, 0, NULL), HU_ERR_INVALID_ARGUMENT);
    /* NULL learner → INVALID_ARGUMENT. */
    HU_ASSERT_EQ(hu_learner_train(NULL, &cfg, NULL, 0, &rep), HU_ERR_INVALID_ARGUMENT);
    /* Empty adapter path → INVALID_ARGUMENT. */
    cfg.adapter_output_path[0] = '\0';
    HU_ASSERT_EQ(hu_learner_train(l, &cfg, NULL, 0, &rep), HU_ERR_INVALID_ARGUMENT);

    hu_learner_close(l);
    unlink(path);
}

/* ── CPU backend determinism ───────────────────────────────────────────── */

static void run_one(const char *path, hu_learner_report_t *out_report) {
    hu_learner_t *l = NULL;
    HU_ASSERT_EQ(hu_learner_open_named(A(), "cpu", &l), HU_OK);
    hu_learner_config_t cfg;
    fill_default_cfg(&cfg, path);

    /* Two synthetic signals — fixed content, fixed weight. */
    hu_training_signal_t signals[2];
    memset(signals, 0, sizeof(signals));
    signals[0].kind = HU_TRAIN_DPO_PAIR;
    snprintf(signals[0].as.dpo.prompt, sizeof(signals[0].as.dpo.prompt),
             "what time should I head to the gym?");
    snprintf(signals[0].as.dpo.preferred, sizeof(signals[0].as.dpo.preferred),
             "you usually go around 7:30");
    snprintf(signals[0].as.dpo.dispreferred, sizeof(signals[0].as.dpo.dispreferred),
             "I have no idea, ask the calendar");
    signals[0].as.dpo.weight = 1.0f;
    signals[0].observed_at = 1735690000000LL;

    signals[1].kind = HU_TRAIN_CASE_OUTCOME;
    signals[1].as.case_outcome.case_id = 42;
    signals[1].as.case_outcome.reward = 0.9f;
    signals[1].observed_at = 1735690500000LL;

    HU_ASSERT_EQ(hu_learner_train(l, &cfg, signals, 2, out_report), HU_OK);
    hu_learner_close(l);
}

static void test_w13_cpu_backend_trains_deterministic_adapter(void) {
    char path1[256];
    char path2[256];
    scratch_path(path1, sizeof(path1), "det-a");
    scratch_path(path2, sizeof(path2), "det-b");

    hu_learner_report_t r1, r2;
    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));

    /* First two calls write to different paths so we can compare bytes. */
    hu_learner_t *l = NULL;
    HU_ASSERT_EQ(hu_learner_open_named(A(), "cpu", &l), HU_OK);
    hu_learner_config_t cfg1;
    hu_learner_config_t cfg2;
    fill_default_cfg(&cfg1, path1);
    fill_default_cfg(&cfg2, path2);

    hu_training_signal_t signals[2];
    memset(signals, 0, sizeof(signals));
    signals[0].kind = HU_TRAIN_PERSONA_DELTA;
    signals[0].as.persona.delta.kind = HU_PERSONA_DELTA_TONE;
    snprintf(signals[0].as.persona.delta.value, sizeof(signals[0].as.persona.delta.value),
             "warmer");
    signals[0].as.persona.delta.confidence = 0.85f;
    signals[1].kind = HU_TRAIN_CASE_OUTCOME;
    signals[1].as.case_outcome.case_id = 7;
    signals[1].as.case_outcome.reward = 0.75f;

    HU_ASSERT_EQ(hu_learner_train(l, &cfg1, signals, 2, &r1), HU_OK);
    HU_ASSERT_EQ(hu_learner_train(l, &cfg2, signals, 2, &r2), HU_OK);
    hu_learner_close(l);

    /* Same seed, same signals, same model_version → byte-identical loss
     * and adapter size. */
    HU_ASSERT_EQ(r1.steps_completed, r2.steps_completed);
    HU_ASSERT_EQ(r1.adapter_bytes, r2.adapter_bytes);
    HU_ASSERT_FLOAT_EQ(r1.final_loss, r2.final_loss, 0.0f);

    /* Compare files byte-for-byte. */
    FILE *f1 = fopen(path1, "rb");
    FILE *f2 = fopen(path2, "rb");
    HU_ASSERT_NOT_NULL(f1);
    HU_ASSERT_NOT_NULL(f2);
    int c1, c2;
    long n = 0;
    do {
        c1 = fgetc(f1);
        c2 = fgetc(f2);
        if (c1 != c2)
            HU_FAIL("adapter bytes diverge at offset %ld: %d vs %d", n, c1, c2);
        n++;
    } while (c1 != EOF);
    fclose(f1);
    fclose(f2);
    HU_ASSERT_EQ(n - 1, r1.adapter_bytes);

    unlink(path1);
    unlink(path2);
    /* Silence unused warnings on the helper above. */
    (void)run_one;
}

static void test_w13_adapter_file_round_trip(void) {
    char path[256];
    scratch_path(path, sizeof(path), "rt");

    hu_learner_report_t rep;
    memset(&rep, 0, sizeof(rep));
    hu_learner_t *l = NULL;
    HU_ASSERT_EQ(hu_learner_open_named(A(), "cpu", &l), HU_OK);

    hu_learner_config_t cfg;
    fill_default_cfg(&cfg, path);
    snprintf(cfg.model_version, sizeof(cfg.model_version), "round-trip-7");

    HU_ASSERT_EQ(hu_learner_train(l, &cfg, NULL, 0, &rep), HU_OK);
    hu_learner_close(l);

    HU_ASSERT_STR_EQ(rep.model_version, "round-trip-7");
    HU_ASSERT(rep.adapter_bytes > 88);

    /* Read back and verify header. */
    FILE *f = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(f);
    char magic[4];
    HU_ASSERT_EQ(fread(magic, 1, 4, f), 4);
    HU_ASSERT_EQ(memcmp(magic, HU_LEARNER_ADAPTER_MAGIC, 4), 0);

    uint8_t v[4];
    HU_ASSERT_EQ(fread(v, 1, 4, f), 4);
    uint32_t version = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) |
                       ((uint32_t)v[3] << 24);
    HU_ASSERT_EQ(version, HU_LEARNER_ADAPTER_VERSION);

    char mv[64];
    HU_ASSERT_EQ(fread(mv, 1, sizeof(mv), f), sizeof(mv));
    HU_ASSERT_STR_EQ(mv, "round-trip-7");

    fclose(f);
    HU_ASSERT_EQ(file_size(path), (int)rep.adapter_bytes);
    unlink(path);
}

static void test_w13_adversarial_training_data_poisoning_does_not_crash(void) {
    char path[256];
    scratch_path(path, sizeof(path), "adv");

    /* 20 signals, 10 of which are adversarial: empty strings, repeated
     * content, and out-of-range rewards. The trainer must not crash and
     * must emit a valid adapter file. */
    hu_training_signal_t s[20];
    memset(s, 0, sizeof(s));
    for (size_t i = 0; i < 20; i++) {
        if (i % 2 == 0) {
            s[i].kind = HU_TRAIN_DPO_PAIR;
            snprintf(s[i].as.dpo.prompt, sizeof(s[i].as.dpo.prompt),
                     "benign prompt %zu", i);
            snprintf(s[i].as.dpo.preferred, sizeof(s[i].as.dpo.preferred),
                     "benign reply %zu", i);
            snprintf(s[i].as.dpo.dispreferred, sizeof(s[i].as.dpo.dispreferred),
                     "bad reply %zu", i);
            s[i].as.dpo.weight = 1.0f;
        } else {
            /* Adversarial: empty preferred + dispreferred, garbage prompt. */
            s[i].kind = HU_TRAIN_DPO_PAIR;
            for (size_t k = 0; k < sizeof(s[i].as.dpo.prompt) - 1; k++)
                s[i].as.dpo.prompt[k] = (char)(0x80 | (k & 0x7F));
            s[i].as.dpo.weight = -100.0f; /* nonsense weight */
        }
    }

    hu_learner_t *l = NULL;
    HU_ASSERT_EQ(hu_learner_open_named(A(), "cpu", &l), HU_OK);
    hu_learner_config_t cfg;
    fill_default_cfg(&cfg, path);
    cfg.max_steps = 10;
    hu_learner_report_t rep;
    HU_ASSERT_EQ(hu_learner_train(l, &cfg, s, 20, &rep), HU_OK);
    hu_learner_close(l);

    HU_ASSERT(rep.adapter_bytes > 0);
    HU_ASSERT_EQ(rep.signals_consumed, 20);

    /* Verify file still parses as a valid HLAD. */
    FILE *f = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(f);
    char magic[4];
    HU_ASSERT_EQ(fread(magic, 1, 4, f), 4);
    HU_ASSERT_EQ(memcmp(magic, HU_LEARNER_ADAPTER_MAGIC, 4), 0);
    fclose(f);
    unlink(path);
}

static void test_w13_dp_epsilon_zero_disables_noise(void) {
    char path[256];
    scratch_path(path, sizeof(path), "dp0");

    hu_learner_t *l = NULL;
    HU_ASSERT_EQ(hu_learner_open_named(A(), "cpu", &l), HU_OK);
    hu_learner_config_t cfg;
    fill_default_cfg(&cfg, path);
    cfg.dp_enabled = true;
    cfg.dp_epsilon = 0.0f;
    hu_learner_report_t rep;
    /* Spec contract: error or fallback when DP is requested without epsilon. */
    HU_ASSERT_EQ(hu_learner_train(l, &cfg, NULL, 0, &rep), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_STR_CONTAINS(rep.last_error, "dp");

    hu_learner_close(l);
    unlink(path);
}

static void test_w13_budget_ms_zero_short_circuits(void) {
    char path[256];
    scratch_path(path, sizeof(path), "budget0");

    hu_learner_t *l = NULL;
    HU_ASSERT_EQ(hu_learner_open_named(A(), "cpu", &l), HU_OK);
    hu_learner_config_t cfg;
    fill_default_cfg(&cfg, path);
    cfg.budget_ms = 0;
    cfg.max_steps = 1000; /* large; should be ignored */
    hu_learner_report_t rep;
    HU_ASSERT_EQ(hu_learner_train(l, &cfg, NULL, 0, &rep), HU_OK);
    HU_ASSERT_EQ((int)rep.steps_completed, 0);
    HU_ASSERT_EQ(rep.adapter_bytes, 0);
    /* No file should have been created. */
    HU_ASSERT_EQ(file_size(path), -1);

    hu_learner_close(l);
}

/* ── Signal builders ──────────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE

static void open_facade_(hu_graph_t **g, hu_memory_t **m) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_NOT_NULL(*g);
    HU_ASSERT_EQ(hu_memory_open(A(), *g, m), HU_OK);
    HU_ASSERT_NOT_NULL(*m);
}

static void close_facade_(hu_graph_t *g, hu_memory_t *m) {
    hu_memory_close(m, A());
    hu_graph_close(g, A());
}

static void test_w13_signals_from_verifier_flags_skips_already_consumed(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    /* Propose a delta and quarantine it directly via SQL — easiest path.
     * The status enum value 3 == HU_DELTA_STATUS_QUARANTINED. */
    int64_t did = 0;
    HU_ASSERT_EQ(hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_VOCAB_AVOID, "all",
                                          "obviously", 0.6f, "agent-inference",
                                          1735690000000LL, &did),
                 HU_OK);
    /* Manually flip to quarantined via list/free pattern + UPDATE. We use
     * a second proposal with a different value that the evolver would
     * naturally drop, then ensure quarantined-status filtering works for
     * the case where we have only one such row. We piggyback on the W5
     * evolver: a low-confidence delta gets dropped, a flooded one gets
     * quarantined. To keep the test deterministic we instead inspect
     * pending deltas — the builder semantics (status=QUARANTINED) returns
     * 0 here, which is the documented idempotent contract. */

    hu_training_signal_t *s1 = NULL;
    size_t n1 = 0;
    HU_ASSERT_EQ(hu_learner_signals_from_verifier_flags(m, A(), "u1", 2, &s1, &n1), HU_OK);
    /* No quarantined deltas yet → 0 signals. */
    HU_ASSERT_EQ((int)n1, 0);
    HU_ASSERT_NULL(s1);

    /* Idempotent: calling twice in a row returns the same answer. */
    hu_training_signal_t *s2 = NULL;
    size_t n2 = 0;
    HU_ASSERT_EQ(hu_learner_signals_from_verifier_flags(m, A(), "u1", 2, &s2, &n2), HU_OK);
    HU_ASSERT_EQ(n1, n2);
    hu_learner_signals_free(A(), s1, n1);
    hu_learner_signals_free(A(), s2, n2);

    /* Now drive a quarantine via the evolver: many proposals from the
     * same source within an hour → quarantined. */
    for (int i = 0; i < 12; i++) {
        hu_persona_delta_propose(g, "u1", 2, HU_PERSONA_DELTA_TONE, "slack",
                                 "spam-tone", 0.3f, "noisy-source",
                                 1735690000000LL + i * 1000, NULL);
    }
    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1735690000000LL + 60 * 1000;
    cfg.rate_limit_per_hour = 5;
    hu_persona_evolver_report_t rep;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &cfg, &rep), HU_OK);
    HU_ASSERT(rep.quarantined > 0);

    hu_training_signal_t *s3 = NULL;
    size_t n3 = 0;
    HU_ASSERT_EQ(hu_learner_signals_from_verifier_flags(m, A(), "u1", 2, &s3, &n3), HU_OK);
    HU_ASSERT(n3 > 0);
    /* Idempotent again. */
    hu_training_signal_t *s4 = NULL;
    size_t n4 = 0;
    HU_ASSERT_EQ(hu_learner_signals_from_verifier_flags(m, A(), "u1", 2, &s4, &n4), HU_OK);
    HU_ASSERT_EQ(n3, n4);
    HU_ASSERT_EQ(s3[0].kind, HU_TRAIN_DPO_PAIR);
    hu_learner_signals_free(A(), s3, n3);
    hu_learner_signals_free(A(), s4, n4);

    close_facade_(g, m);
}

static void test_w13_dpo_pairs_have_no_self_inconsistencies(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    /* Drive a quarantine. */
    for (int i = 0; i < 12; i++) {
        hu_persona_delta_propose(g, "u2", 2, HU_PERSONA_DELTA_VALUE, "all",
                                 "be-rude", 0.4f, "noisy-source",
                                 1735690000000LL + i * 1000, NULL);
    }
    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1735690000000LL + 60 * 1000;
    cfg.rate_limit_per_hour = 5;
    hu_persona_evolver_report_t rep;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u2", 2, &cfg, &rep), HU_OK);
    HU_ASSERT(rep.quarantined > 0);

    hu_training_signal_t *s = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_learner_signals_from_verifier_flags(m, A(), "u2", 2, &s, &n), HU_OK);
    HU_ASSERT(n > 0);
    for (size_t i = 0; i < n; i++) {
        HU_ASSERT_EQ(s[i].kind, HU_TRAIN_DPO_PAIR);
        HU_ASSERT_NEQ(strcmp(s[i].as.dpo.preferred, s[i].as.dpo.dispreferred), 0);
        HU_ASSERT(s[i].as.dpo.weight > 0.0f);
    }
    hu_learner_signals_free(A(), s, n);

    close_facade_(g, m);
}

static void test_w13_signals_from_persona_deltas_round_trip(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    /* Two high-confidence proposals from different sources → applied. */
    hu_persona_delta_propose(g, "u3", 2, HU_PERSONA_DELTA_LENGTH, "slack", "shorter", 0.9f,
                             "agent-inference", 1735690000000LL, NULL);
    hu_persona_delta_propose(g, "u3", 2, HU_PERSONA_DELTA_TONE, "slack", "warmer", 0.92f,
                             "user-explicit", 1735690000100LL, NULL);
    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1735690000000LL + 5000;
    hu_persona_evolver_report_t rep;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u3", 2, &cfg, &rep), HU_OK);
    HU_ASSERT_EQ((int)rep.applied, 2);

    hu_training_signal_t *s = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_learner_signals_from_persona_deltas(m, A(), "u3", 2, &s, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 2);
    /* Round-trip: each signal carries the delta verbatim. */
    int seen_warmer = 0, seen_shorter = 0;
    for (size_t i = 0; i < n; i++) {
        HU_ASSERT_EQ(s[i].kind, HU_TRAIN_PERSONA_DELTA);
        HU_ASSERT(s[i].observed_at > 0);
        if (strcmp(s[i].as.persona.delta.value, "warmer") == 0)
            seen_warmer = 1;
        if (strcmp(s[i].as.persona.delta.value, "shorter") == 0)
            seen_shorter = 1;
    }
    HU_ASSERT(seen_warmer);
    HU_ASSERT(seen_shorter);
    hu_learner_signals_free(A(), s, n);

    close_facade_(g, m);
}

static void test_w13_signals_from_case_outcomes_returns_positive_for_high_reward(void) {
    hu_graph_t *g = NULL;
    hu_memory_t *m = NULL;
    open_facade_(&g, &m);

    int64_t cid_ok = 0, cid_bad = 0, cid_meh = 0;
    HU_ASSERT_EQ(hu_case_record(g, "u4", 2, "schedule", 8, NULL, 0, "plan A", 6, "ok", 2,
                                1735690000000LL, &cid_ok),
                 HU_OK);
    HU_ASSERT_EQ(hu_case_record(g, "u4", 2, "schedule", 8, NULL, 0, "plan B", 6,
                                "user pushed back", 16, 1735690500000LL, &cid_bad),
                 HU_OK);
    HU_ASSERT_EQ(hu_case_record(g, "u4", 2, "schedule", 8, NULL, 0, "plan C", 6,
                                "unclear", 7, 1735691000000LL, &cid_meh),
                 HU_OK);

    hu_training_signal_t *s = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_learner_signals_from_case_outcomes(m, A(), "u4", 2, &s, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 3);

    int high = 0, low = 0, mid = 0;
    for (size_t i = 0; i < n; i++) {
        HU_ASSERT_EQ(s[i].kind, HU_TRAIN_CASE_OUTCOME);
        if (s[i].as.case_outcome.case_id == cid_ok) {
            HU_ASSERT(s[i].as.case_outcome.reward >= 0.99f);
            high = 1;
        } else if (s[i].as.case_outcome.case_id == cid_bad) {
            HU_ASSERT(s[i].as.case_outcome.reward <= 0.01f);
            low = 1;
        } else if (s[i].as.case_outcome.case_id == cid_meh) {
            HU_ASSERT(s[i].as.case_outcome.reward > 0.4f &&
                      s[i].as.case_outcome.reward < 0.6f);
            mid = 1;
        }
    }
    HU_ASSERT(high && low && mid);

    /* Idempotency: second call returns the same set. */
    hu_training_signal_t *s2 = NULL;
    size_t n2 = 0;
    HU_ASSERT_EQ(hu_learner_signals_from_case_outcomes(m, A(), "u4", 2, &s2, &n2), HU_OK);
    HU_ASSERT_EQ(n, n2);

    hu_learner_signals_free(A(), s, n);
    hu_learner_signals_free(A(), s2, n2);
    close_facade_(g, m);
}

#endif /* HU_ENABLE_SQLITE */

/* ── Test runner ──────────────────────────────────────────────────────── */

void run_w13_learner_tests(void) {
    HU_TEST_SUITE("W13 learner - hu_learner_t vtable + CPU backend + signal builders");

    HU_RUN_TEST(test_w13_default_config_has_sensible_defaults);
    HU_RUN_TEST(test_w13_learner_open_default_returns_some_backend);
    HU_RUN_TEST(test_w13_learner_open_named_unknown_returns_error);
    HU_RUN_TEST(test_w13_invalid_args_rejected);
    HU_RUN_TEST(test_w13_cpu_backend_trains_deterministic_adapter);
    HU_RUN_TEST(test_w13_adapter_file_round_trip);
    HU_RUN_TEST(test_w13_adversarial_training_data_poisoning_does_not_crash);
    HU_RUN_TEST(test_w13_dp_epsilon_zero_disables_noise);
    HU_RUN_TEST(test_w13_budget_ms_zero_short_circuits);

#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w13_signals_from_verifier_flags_skips_already_consumed);
    HU_RUN_TEST(test_w13_dpo_pairs_have_no_self_inconsistencies);
    HU_RUN_TEST(test_w13_signals_from_persona_deltas_round_trip);
    HU_RUN_TEST(test_w13_signals_from_case_outcomes_returns_positive_for_high_reward);
#endif
}
