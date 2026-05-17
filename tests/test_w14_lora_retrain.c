/* US-7.5 — Nightly LoRA re-train cron runner tests.
 *
 * Covers the 5 AC plus the lora_retrain status block parse round-trip and
 * the D3 SKIP contract. Every subprocess invocation is mocked via
 * `test_run_subprocess`; no real Python is ever forked. */

#include "human/agent/scheduler.h"
#include "human/agent/world_model_bridge.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/ml/lora_retrain_runner.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Subprocess hook capture machinery ────────────────────────────────── */

#define MAX_CAPTURES 8
#define MAX_ARGV     16

typedef struct {
    int n_calls;
    char argv[MAX_CAPTURES][MAX_ARGV][128];
    int argc[MAX_CAPTURES];
    /* Queued responses, in call order. */
    int queued_exit[MAX_CAPTURES];
    char queued_stdout[MAX_CAPTURES][2048];
    int n_queued;
    int next_response;
} subprocess_capture_t;

static hu_error_t capture_subprocess(const char *const argv[],
                                     hu_lora_retrain_proc_result_t *result, void *ud) {
    subprocess_capture_t *cap = (subprocess_capture_t *)ud;
    if (cap->n_calls >= MAX_CAPTURES)
        return HU_ERR_INVALID_ARGUMENT;
    int slot = cap->n_calls++;
    int ac = 0;
    while (argv[ac] && ac < MAX_ARGV) {
        snprintf(cap->argv[slot][ac], sizeof(cap->argv[slot][ac]), "%s", argv[ac]);
        ac++;
    }
    cap->argc[slot] = ac;

    /* Respond with the next queued reply (or empty PASS if exhausted). */
    int idx = (cap->next_response < cap->n_queued) ? cap->next_response : cap->n_queued - 1;
    if (idx < 0) {
        result->exit_code = 0;
        result->stdout_buf[0] = '\0';
        result->stdout_len = 0;
    } else {
        result->exit_code = cap->queued_exit[idx];
        snprintf(result->stdout_buf, sizeof(result->stdout_buf), "%s", cap->queued_stdout[idx]);
        result->stdout_len = strlen(result->stdout_buf);
    }
    cap->next_response++;
    return HU_OK;
}

static void queue_response(subprocess_capture_t *cap, int exit_code, const char *stdout_str) {
    int slot = cap->n_queued++;
    cap->queued_exit[slot] = exit_code;
    snprintf(cap->queued_stdout[slot], sizeof(cap->queued_stdout[slot]), "%s",
             stdout_str ? stdout_str : "");
}

/* ── Event capture ───────────────────────────────────────────────────── */

typedef struct {
    int n_events;
    char names[16][96];
    char payloads[16][512];
} event_capture_t;

static void capture_event(const char *event, const char *payload, void *ud) {
    event_capture_t *ec = (event_capture_t *)ud;
    if (ec->n_events >= 16)
        return;
    snprintf(ec->names[ec->n_events], sizeof(ec->names[0]), "%s", event ? event : "");
    snprintf(ec->payloads[ec->n_events], sizeof(ec->payloads[0]), "%s", payload ? payload : "");
    ec->n_events++;
}

static int event_seen(const event_capture_t *ec, const char *name) {
    for (int i = 0; i < ec->n_events; i++) {
        if (strcmp(ec->names[i], name) == 0)
            return 1;
    }
    return 0;
}

static int argv_contains(subprocess_capture_t *cap, int call_idx, const char *token) {
    if (call_idx >= cap->n_calls)
        return 0;
    for (int i = 0; i < cap->argc[call_idx]; i++) {
        if (strcmp(cap->argv[call_idx][i], token) == 0)
            return 1;
    }
    return 0;
}

/* ── Test setup helper ───────────────────────────────────────────────── */

static void setup_ctx(hu_lora_retrain_ctx_t *ctx, subprocess_capture_t *cap, event_capture_t *ec,
                      const char *candidate_dir, const char *current_symlink) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->candidate_dir = candidate_dir;
    ctx->current_symlink = current_symlink;
    ctx->test_run_subprocess = capture_subprocess;
    ctx->test_subprocess_ud = cap;
    ctx->emit_event = capture_event;
    ctx->emit_user_data = ec;
    /* No pidfile by default — tests opt in by setting ctx->pidfile_path. */
    ctx->pidfile_path = NULL;
}

static void make_spec(hu_job_spec_t *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->kind = HU_JOB_LORA_RETRAIN_NIGHTLY;
}

/* ── AC-7.5.1: enqueue + finetune argv shape ─────────────────────────── */

static void test_retrain_enqueues_and_invokes_finetune(void) {
    subprocess_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    event_capture_t ec;
    memset(&ec, 0, sizeof(ec));
    hu_lora_retrain_ctx_t ctx;
    setup_ctx(&ctx, &cap, &ec, "/tmp/test_candidate", "/tmp/test_current");

    /* Pair-count probe: returns 7 pairs. */
    queue_response(&cap, 0, "{\"pairs\":7}");
    /* finetune: success. */
    queue_response(&cap, 0, "");
    /* gate: PASS. */
    queue_response(&cap, 0, "{\"verdict\":\"PASS\",\"delta\":0.12}");

    hu_job_spec_t spec;
    make_spec(&spec);
    HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx), HU_OK);

    /* Call 0 = mine-corrections --count-only */
    HU_ASSERT_GE(cap.n_calls, 3);
    HU_ASSERT(argv_contains(&cap, 0, "mine-corrections"));
    HU_ASSERT(argv_contains(&cap, 0, "--count-only"));

    /* Call 1 = finetune-gemma.py invocation */
    HU_ASSERT(argv_contains(&cap, 1, "--dpo"));
    HU_ASSERT(argv_contains(&cap, 1, "--from-corrections"));
    HU_ASSERT(argv_contains(&cap, 1, "--no-restart-server"));
    HU_ASSERT(argv_contains(&cap, 1, "--no-version"));

    /* AC: lora_retrain_scheduled event emitted with pair count. */
    HU_ASSERT(event_seen(&ec, "lora_retrain_scheduled"));
    HU_ASSERT(event_seen(&ec, "lora_retrain_promoted"));
    HU_ASSERT_EQ(ctx.last_outcome, HU_LORA_RETRAIN_OUTCOME_PROMOTED);
    HU_ASSERT_EQ((long long)ctx.last_pairs_consumed, 7LL);

    /* Cleanup symlink so subsequent tests start clean. */
    (void)unlink("/tmp/test_current");
}

/* ── AC-7.5.2 PASS: promote happens, symlink updated ─────────────────── */

static void test_retrain_promotes_on_pass_skips_on_fail(void) {
    /* PASS half: */
    {
        subprocess_capture_t cap;
        memset(&cap, 0, sizeof(cap));
        event_capture_t ec;
        memset(&ec, 0, sizeof(ec));
        hu_lora_retrain_ctx_t ctx;
        setup_ctx(&ctx, &cap, &ec, "/tmp/test_candidate_pass", "/tmp/test_current_pass");

        queue_response(&cap, 0, "{\"pairs\":3}");
        queue_response(&cap, 0, "");
        queue_response(&cap, 0, "{\"verdict\":\"PASS\",\"delta\":0.08}");

        hu_job_spec_t spec;
        make_spec(&spec);
        HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx), HU_OK);
        HU_ASSERT_EQ(ctx.last_outcome, HU_LORA_RETRAIN_OUTCOME_PROMOTED);
        HU_ASSERT(event_seen(&ec, "lora_retrain_promoted"));

        /* The symlink should now exist and point at the candidate. */
        char buf[256] = {0};
        ssize_t n = readlink("/tmp/test_current_pass", buf, sizeof(buf) - 1);
        HU_ASSERT_GT(n, 0);
        buf[n] = '\0';
        HU_ASSERT_STR_EQ(buf, "/tmp/test_candidate_pass");
        (void)unlink("/tmp/test_current_pass");
    }
    /* FAIL half: */
    {
        subprocess_capture_t cap;
        memset(&cap, 0, sizeof(cap));
        event_capture_t ec;
        memset(&ec, 0, sizeof(ec));
        hu_lora_retrain_ctx_t ctx;
        setup_ctx(&ctx, &cap, &ec, "/tmp/test_candidate_fail", "/tmp/test_current_fail");

        queue_response(&cap, 0, "{\"pairs\":5}");
        queue_response(&cap, 0, "");
        /* Gate ran cleanly (exit 0) but produced verdict=FAIL. Per FIX-3 this
         * routes to SKIPPED_GATE_FAIL; non-zero gate exits route to FAILED
         * and are covered by test_retrain_gate_exit_nonzero_routes_to_failed. */
        queue_response(&cap, 0, "{\"verdict\":\"FAIL\",\"delta\":-0.02}");

        hu_job_spec_t spec;
        make_spec(&spec);
        HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx), HU_OK);
        HU_ASSERT_EQ(ctx.last_outcome, HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL);

        /* lora_retrain_skipped event with verdict=FAIL */
        int found = 0;
        for (int i = 0; i < ec.n_events; i++) {
            if (strcmp(ec.names[i], "lora_retrain_skipped") == 0) {
                found = 1;
                HU_ASSERT_STR_CONTAINS(ec.payloads[i], "FAIL");
            }
        }
        HU_ASSERT(found);

        /* No promote subprocess; we only expect 3 calls (probe, finetune, gate). */
        HU_ASSERT_EQ(cap.n_calls, 3);

        /* Symlink does NOT exist — never created. */
        char dummy[64];
        HU_ASSERT_LT(readlink("/tmp/test_current_fail", dummy, sizeof(dummy)), 0);
    }
}

/* ── AC-7.5.2 SKIP variant: D3 contract (judgment SKIP ≠ PASS) ───────── */

static void test_retrain_treats_judgment_skip_as_not_pass(void) {
    subprocess_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    event_capture_t ec;
    memset(&ec, 0, sizeof(ec));
    hu_lora_retrain_ctx_t ctx;
    setup_ctx(&ctx, &cap, &ec, "/tmp/test_candidate_skip", "/tmp/test_current_skip");

    queue_response(&cap, 0, "{\"pairs\":4}");
    queue_response(&cap, 0, "");
    /* CRITICAL: SKIP with exit_code=0 (US-7.6 dormant gate). Must NOT promote. */
    queue_response(&cap, 0, "{\"verdict\":\"SKIP\",\"reason\":\"judgment_dormant\"}");

    hu_job_spec_t spec;
    make_spec(&spec);
    HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx), HU_OK);
    HU_ASSERT_EQ(ctx.last_outcome, HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL);
    HU_ASSERT(!event_seen(&ec, "lora_retrain_promoted"));

    /* Verify symlink NOT created — SKIP must not promote. */
    char dummy[64];
    HU_ASSERT_LT(readlink("/tmp/test_current_skip", dummy, sizeof(dummy)), 0);
}

/* ── AC-7.5.3: finetune failure preserves current adapter ────────────── */

static void test_retrain_failure_preserves_adapter(void) {
    subprocess_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    event_capture_t ec;
    memset(&ec, 0, sizeof(ec));
    hu_lora_retrain_ctx_t ctx;
    setup_ctx(&ctx, &cap, &ec, "/tmp/test_candidate_x", "/tmp/test_current_x");

    queue_response(&cap, 0, "{\"pairs\":12}");
    /* finetune crashes (exit code 137 = SIGKILL'd by OOM). */
    queue_response(&cap, 137, "");
    /* Gate would be next but should never be called. */
    queue_response(&cap, 0, "{\"verdict\":\"PASS\"}");

    hu_job_spec_t spec;
    make_spec(&spec);
    HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx), HU_OK);
    HU_ASSERT_EQ(ctx.last_outcome, HU_LORA_RETRAIN_OUTCOME_FAILED);
    HU_ASSERT_EQ(ctx.last_exit_code, 137);

    /* failed event with exit_code=137. */
    int found = 0;
    for (int i = 0; i < ec.n_events; i++) {
        if (strcmp(ec.names[i], "lora_retrain_failed") == 0) {
            found = 1;
            HU_ASSERT_STR_CONTAINS(ec.payloads[i], "137");
        }
    }
    HU_ASSERT(found);

    /* Only 2 subprocess calls (probe + failed finetune). Gate never invoked. */
    HU_ASSERT_EQ(cap.n_calls, 2);

    /* Current symlink preserved (never existed in this test → also not
     * created). */
    char dummy[64];
    HU_ASSERT_LT(readlink("/tmp/test_current_x", dummy, sizeof(dummy)), 0);
}

/* ── AC-7.5.4: empty-delta → skipped_no_new_data ─────────────────────── */

static void test_retrain_skipped_on_empty_delta(void) {
    subprocess_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    event_capture_t ec;
    memset(&ec, 0, sizeof(ec));
    hu_lora_retrain_ctx_t ctx;
    setup_ctx(&ctx, &cap, &ec, "/tmp/test_candidate_e", "/tmp/test_current_e");

    /* Probe returns 0 pairs. */
    queue_response(&cap, 0, "{\"pairs\":0}");
    /* These would be the next steps; they MUST NOT be invoked. */
    queue_response(&cap, 0, "");
    queue_response(&cap, 0, "{\"verdict\":\"PASS\"}");

    hu_job_spec_t spec;
    make_spec(&spec);
    HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx), HU_OK);
    HU_ASSERT_EQ(ctx.last_outcome, HU_LORA_RETRAIN_OUTCOME_SKIPPED_NO_NEW_DATA);
    HU_ASSERT(event_seen(&ec, "lora_retrain_skipped_no_new_data"));
    HU_ASSERT(!event_seen(&ec, "lora_retrain_promoted"));
    HU_ASSERT(!event_seen(&ec, "lora_retrain_failed"));

    /* Only the probe ran. */
    HU_ASSERT_EQ(cap.n_calls, 1);
}

/* ── PID single-flight ──────────────────────────────────────────────── */

static void test_retrain_skipped_if_pidfile_held(void) {
    /* Pre-create the PID file with a live PID (our own). */
    const char *pf = "/tmp/test_lora_retrain.pid";
    (void)unlink(pf);
    FILE *fp = fopen(pf, "w");
    HU_ASSERT_NOT_NULL(fp);
    fprintf(fp, "%ld\n", (long)getpid());
    fclose(fp);

    subprocess_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    event_capture_t ec;
    memset(&ec, 0, sizeof(ec));
    hu_lora_retrain_ctx_t ctx;
    setup_ctx(&ctx, &cap, &ec, "/tmp/test_candidate_p", "/tmp/test_current_p");
    ctx.pidfile_path = pf;

    /* Queue responses we expect NOT to be consumed. */
    queue_response(&cap, 0, "{\"pairs\":5}");

    hu_job_spec_t spec;
    make_spec(&spec);
    HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx), HU_OK);
    HU_ASSERT_EQ(ctx.last_outcome, HU_LORA_RETRAIN_OUTCOME_SKIPPED_ALREADY_RUNNING);
    HU_ASSERT_EQ(cap.n_calls, 0);
    HU_ASSERT(event_seen(&ec, "lora_retrain_skipped_already_running"));

    (void)unlink(pf);
}

/* ── AC-7.5.5: status block parse + back-compat ─────────────────────── */

static void test_lora_retrain_status_parse_round_trip(void) {
    /* Forward: present block parses cleanly. */
    const char *json_with = "{\n"
                            "  \"jobs_pending\": 0,\n"
                            "  \"jobs_completed_today\": 1,\n"
                            "  \"battery_pct\": 80,\n"
                            "  \"on_ac_power\": true,\n"
                            "  \"updated_epoch\": 1700000000,\n"
                            "  \"lora_retrain\": {\n"
                            "    \"last_run_ts\": 1715800000,\n"
                            "    \"last_outcome\": \"promoted\",\n"
                            "    \"pairs_consumed\": 42\n"
                            "  }\n"
                            "}\n";
    long long ts = 0;
    hu_lora_retrain_outcome_t oc = HU_LORA_RETRAIN_OUTCOME_UNKNOWN;
    unsigned long long pc = 0;
    HU_ASSERT_EQ(hu_lora_retrain_status_parse(json_with, &ts, &oc, &pc), HU_OK);
    HU_ASSERT_EQ(ts, 1715800000LL);
    HU_ASSERT_EQ(oc, HU_LORA_RETRAIN_OUTCOME_PROMOTED);
    HU_ASSERT_EQ((long long)pc, 42LL);

    /* Backward: missing block → HU_ERR_NOT_FOUND. */
    const char *json_without = "{\n"
                               "  \"jobs_pending\": 0,\n"
                               "  \"jobs_completed_today\": 0,\n"
                               "  \"battery_pct\": 50,\n"
                               "  \"on_ac_power\": false,\n"
                               "  \"updated_epoch\": 1700000000\n"
                               "}\n";
    long long ts2 = 999;
    hu_lora_retrain_outcome_t oc2 = HU_LORA_RETRAIN_OUTCOME_PROMOTED;
    unsigned long long pc2 = 999;
    HU_ASSERT_EQ(hu_lora_retrain_status_parse(json_without, &ts2, &oc2, &pc2), HU_ERR_NOT_FOUND);

    /* All outcome string round-trips. */
    HU_ASSERT_EQ(hu_lora_retrain_outcome_from_str("promoted"), HU_LORA_RETRAIN_OUTCOME_PROMOTED);
    HU_ASSERT_EQ(hu_lora_retrain_outcome_from_str("failed"), HU_LORA_RETRAIN_OUTCOME_FAILED);
    HU_ASSERT_EQ(hu_lora_retrain_outcome_from_str("skipped_no_new_data"),
                 HU_LORA_RETRAIN_OUTCOME_SKIPPED_NO_NEW_DATA);
    HU_ASSERT_EQ(hu_lora_retrain_outcome_from_str("skipped_gate_fail"),
                 HU_LORA_RETRAIN_OUTCOME_SKIPPED_GATE_FAIL);
    HU_ASSERT_STR_EQ(hu_lora_retrain_outcome_str(HU_LORA_RETRAIN_OUTCOME_PROMOTED), "promoted");
    HU_ASSERT_STR_EQ(hu_lora_retrain_outcome_str(HU_LORA_RETRAIN_OUTCOME_FAILED), "failed");
}

/* ── FIX-2: probe parse failure routes to FAILED (not no-new-data) ────── */

static void test_retrain_probe_malformed_routes_to_failed(void) {
    subprocess_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    event_capture_t ec;
    memset(&ec, 0, sizeof(ec));
    hu_lora_retrain_ctx_t ctx;
    setup_ctx(&ctx, &cap, &ec, "/tmp/test_candidate_m", "/tmp/test_current_m");

    /* Malformed probe stdout (no "pairs" key). retrain_parse_pairs → -1.
     * Must route to FAILED with lora_retrain_probe_failed, NOT
     * SKIPPED_NO_NEW_DATA. */
    queue_response(&cap, 0, "{\"oops\":\"no pairs key\"}");
    /* Queued downstream responses MUST NOT be consumed. */
    queue_response(&cap, 0, "");
    queue_response(&cap, 0, "{\"verdict\":\"PASS\"}");

    hu_job_spec_t spec;
    make_spec(&spec);
    HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx), HU_OK);
    HU_ASSERT_EQ(ctx.last_outcome, HU_LORA_RETRAIN_OUTCOME_FAILED);
    HU_ASSERT(event_seen(&ec, "lora_retrain_probe_failed"));
    HU_ASSERT(!event_seen(&ec, "lora_retrain_skipped_no_new_data"));
    /* Only the probe ran; finetune/gate must not have been invoked. */
    HU_ASSERT_EQ(cap.n_calls, 1);

    /* Sanity: a value of 0 still maps to SKIPPED_NO_NEW_DATA (the other
     * branch of FIX-2). */
    subprocess_capture_t cap2;
    memset(&cap2, 0, sizeof(cap2));
    event_capture_t ec2;
    memset(&ec2, 0, sizeof(ec2));
    hu_lora_retrain_ctx_t ctx2;
    setup_ctx(&ctx2, &cap2, &ec2, "/tmp/test_candidate_z", "/tmp/test_current_z");
    queue_response(&cap2, 0, "{\"pairs\":0}");
    HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx2), HU_OK);
    HU_ASSERT_EQ(ctx2.last_outcome, HU_LORA_RETRAIN_OUTCOME_SKIPPED_NO_NEW_DATA);
    HU_ASSERT(event_seen(&ec2, "lora_retrain_skipped_no_new_data"));
    HU_ASSERT(!event_seen(&ec2, "lora_retrain_probe_failed"));
}

/* ── FIX-3: gate non-zero exit routes to FAILED, not SKIPPED_GATE_FAIL ── */

static void test_retrain_gate_exit_nonzero_routes_to_failed(void) {
    subprocess_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    event_capture_t ec;
    memset(&ec, 0, sizeof(ec));
    hu_lora_retrain_ctx_t ctx;
    setup_ctx(&ctx, &cap, &ec, "/tmp/test_candidate_g", "/tmp/test_current_g");

    queue_response(&cap, 0, "{\"pairs\":9}");
    queue_response(&cap, 0, "");
    /* Gate crashes with exit code 2; stdout content is irrelevant. */
    queue_response(&cap, 2, "{\"verdict\":\"PASS\"}");

    hu_job_spec_t spec;
    make_spec(&spec);
    HU_ASSERT_EQ(hu_lora_retrain_runner(NULL, &spec, 0, &ctx), HU_OK);
    /* Non-zero gate exit ⇒ FAILED with exit_code captured. */
    HU_ASSERT_EQ(ctx.last_outcome, HU_LORA_RETRAIN_OUTCOME_FAILED);
    HU_ASSERT_EQ(ctx.last_exit_code, 2);
    HU_ASSERT(event_seen(&ec, "lora_retrain_failed"));
    HU_ASSERT(!event_seen(&ec, "lora_retrain_skipped"));
    HU_ASSERT(!event_seen(&ec, "lora_retrain_promoted"));

    /* The failed event payload identifies the gate step and the exit code. */
    int verified = 0;
    for (int i = 0; i < ec.n_events; i++) {
        if (strcmp(ec.names[i], "lora_retrain_failed") == 0) {
            verified = 1;
            HU_ASSERT_STR_CONTAINS(ec.payloads[i], "\"step\":\"gate\"");
            HU_ASSERT_STR_CONTAINS(ec.payloads[i], "\"exit_code\":2");
        }
    }
    HU_ASSERT(verified);

    /* No symlink was created (no promote ran). */
    char dummy[64];
    HU_ASSERT_LT(readlink("/tmp/test_current_g", dummy, sizeof(dummy)), 0);
}

/* ── FIX-1: enqueue helper drives the scheduler with the correct spec ── */

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_enq_alloc;
static hu_allocator_t *enq_alloc(void) {
    g_enq_alloc = hu_system_allocator();
    return &g_enq_alloc;
}

/* Spec captured by the tick-time runner so the test can inspect what the
 * scheduler actually dispatched (per AC-7.5.1). */
typedef struct {
    int invoked;
    hu_job_spec_t spec;
} enq_capture_t;

static hu_error_t enq_capture_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                     int64_t budget_ms, void *user_data) {
    (void)m;
    (void)budget_ms;
    enq_capture_t *c = (enq_capture_t *)user_data;
    if (!c)
        return HU_OK;
    c->invoked++;
    c->spec = *spec;
    return HU_OK;
}

static void test_enqueue_helper_sets_correct_spec(void) {
    /* The scheduler's idle / AC probes honor these env vars under HU_IS_TEST. */
    setenv("HU_TEST_LOAD_PCT", "10", 1);
    setenv("HU_TEST_ON_AC", "1", 1);
    setenv("HU_TEST_BATTERY_PCT", "100", 1);

    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    HU_ASSERT_EQ(hu_graph_open(enq_alloc(), NULL, 0, &g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(g, enq_alloc(), &f), HU_OK);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, enq_alloc(), &s), HU_OK);

    /* Swap in a capturing runner for HU_JOB_LORA_RETRAIN_NIGHTLY. The bridge
     * itself only registers the real runner when register_lora_retrain_runner
     * is called; we deliberately skip that so our capture stays bound. */
    enq_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    HU_ASSERT_EQ(hu_scheduler_register_runner(hu_w14_scheduler_inner(s),
                                              HU_JOB_LORA_RETRAIN_NIGHTLY, enq_capture_runner,
                                              &cap),
                 HU_OK);

    /* Enqueue via the helper under test. */
    int64_t now_ms = 5000;
    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_lora_retrain_nightly(s, now_ms, 0), HU_OK);

    /* A pending job is now in the queue. */
    size_t pending = 0;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT_GE(pending, (size_t)1);

    /* Tick — the scheduler should dispatch our capture runner because the
     * idle + AC probes return values that satisfy requires_idle and
     * requires_ac_power. */
    HU_ASSERT_EQ(hu_w14_scheduler_tick(s, now_ms + 100), HU_OK);

    /* Verify dispatch happened and the dispatched spec matches the
     * contract documented for `enqueue_lora_retrain_nightly`. (The
     * scheduler clamps `budget_ms` against `HU_SCHED_TOTAL_BUDGET_MS`
     * before dispatch, so we assert the semantic-spec fields that are
     * preserved verbatim.) */
    HU_ASSERT_EQ(cap.invoked, 1);
    HU_ASSERT_EQ(cap.spec.kind, HU_JOB_LORA_RETRAIN_NIGHTLY);
    HU_ASSERT(cap.spec.requires_idle);
    HU_ASSERT(cap.spec.requires_ac_power);
    HU_ASSERT_EQ(cap.spec.interval_sec, 86400);

    /* NULL-arg defenses. */
    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_lora_retrain_nightly(NULL, 0, 0),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w14_scheduler_register_lora_retrain_runner(NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);

    hu_w14_scheduler_close(s, enq_alloc());
    hu_w7_facade_close(f, enq_alloc());
    hu_graph_close(g, enq_alloc());
}

#else /* !HU_ENABLE_SQLITE */

static void test_enqueue_helper_sets_correct_spec(void) {
    /* W14 scheduler requires SQLite; skip on minimal builds. */
}

#endif

/* ── Suite entry ─────────────────────────────────────────────────────── */

void run_w14_lora_retrain_tests(void) {
    HU_TEST_SUITE("w14_lora_retrain");
    HU_RUN_TEST(test_retrain_enqueues_and_invokes_finetune);
    HU_RUN_TEST(test_retrain_promotes_on_pass_skips_on_fail);
    HU_RUN_TEST(test_retrain_treats_judgment_skip_as_not_pass);
    HU_RUN_TEST(test_retrain_failure_preserves_adapter);
    HU_RUN_TEST(test_retrain_skipped_on_empty_delta);
    HU_RUN_TEST(test_retrain_skipped_if_pidfile_held);
    HU_RUN_TEST(test_lora_retrain_status_parse_round_trip);
    HU_RUN_TEST(test_retrain_probe_malformed_routes_to_failed);
    HU_RUN_TEST(test_retrain_gate_exit_nonzero_routes_to_failed);
    HU_RUN_TEST(test_enqueue_helper_sets_correct_spec);
}
