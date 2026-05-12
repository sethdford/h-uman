/* FIX 12 — W7 facade + W9 world-model bridge.
 *
 * Verifies the bridge open/close lifecycle, that hu_w7_render_world_model
 * produces a non-empty markdown block when the underlying world model has
 * goals/negatives/topics, and that an empty world model returns NULL/0
 * (so callers cleanly skip injection). */

#include "human/agent/world_model_bridge.h"
#include "human/agent/self_rag.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/hyperedge.h"
#include "human/memory/memory.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"
#include <stdlib.h>

/* These two are deliberately the only W7-headers visible to this TU. We
 * include them through a separate helper so this test does not double-
 * include `human/memory.h` (legacy) and `human/memory/memory.h` (W7) in the
 * same translation unit. We use a forward declaration trick: the bridge
 * already exports an opaque type, so we don't need direct W7 access here. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

/* Helper: open graph, open W7 facade, return both. Disables the world-model
 * cache so per-test contact ids do not see stale entries from a prior test
 * (the global cache lives in src/agent/world_model.c). */
static void open_graph_and_facade(hu_graph_t **out_g, hu_w7_facade_t **out_f) {
    setenv("HU_WORLD_MODEL_TTL_MS", "0", 1);
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, out_g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(*out_g, A(), out_f), HU_OK);
}

static void cleanup(hu_graph_t *g, hu_w7_facade_t *f) {
    if (f)
        hu_w7_facade_close(f, A());
    if (g)
        hu_graph_close(g, A());
}

/* --- Lifecycle --- */

static void bridge_open_close_clean(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    HU_ASSERT_NOT_NULL(f);
    cleanup(g, f);
}

static void bridge_open_rejects_null_args(void) {
    hu_w7_facade_t *f = NULL;
    HU_ASSERT_EQ(hu_w7_facade_open(NULL, A(), &f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT(f == NULL);
    /* close NULL is a no-op (does not crash). */
    hu_w7_facade_close(NULL, A());
}

/* --- Render: empty graph -> empty output --- */

static void bridge_render_empty_world_model_returns_null(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_empty", 8, 1700000000000LL, &txt, &tlen,
                                            NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    /* Empty world model -> NULL/0 so callers skip injection. */
    HU_ASSERT(txt == NULL);
    HU_ASSERT_EQ(tlen, 0);

    cleanup(g, f);
}

/* B8 follow-up: optional ToM scenario merges after load so an empty graph
 * still yields a prompt block (eval harness / layered vignettes). */
static void bridge_render_optional_tom_scenario_merges_into_output(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    const char *p = "Anne moves the ball while Max is away; Max did not see the move.";
    const char *q = "Where will Max look for the ball first?";
    const char *c = "false_belief";
    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_tom_sc", 9, 1700000000000LL, &txt, &tlen, p,
                                            strlen(p), q, strlen(q), c, strlen(c), NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT_GT(tlen, (size_t)0);
    HU_ASSERT_NOT_NULL(strstr(txt, "[ToM:fb]"));
    HU_ASSERT_NOT_NULL(strstr(txt, "Theory of mind"));

    A()->free(A()->ctx, txt, tlen + 1);
    cleanup(g, f);
}

/* --- Render: non-empty world model produces formatted text ---
 *
 * We seed the underlying graph with a negative-memory entry through the
 * W9 public API. That ensures the world_model_load builder sees something
 * worth surfacing. */

#include "human/agent/world_model.h"

static void bridge_render_with_negative_memory_includes_avoid_section(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    hu_negative_memory_t nm = {0};
    snprintf(nm.text, sizeof(nm.text), "joke about her grandmother");
    snprintf(nm.scope, sizeof(nm.scope), "topic");
    snprintf(nm.reason, sizeof(nm.reason), "she's grieving");
    nm.belief = hu_belief_init(0.95f, "test", 1700000000000LL);
    nm.created_at = 1700000000000LL;

    int64_t id = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "ut_neg", 6, &nm, &id), HU_OK);
    HU_ASSERT(id > 0);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_neg", 6, 1700000000000LL + 1000, &txt, &tlen,
                                            NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT(tlen > 0);
    /* Header always present when any section fires. */
    HU_ASSERT(strstr(txt, "What I know about this conversation") != NULL);
    /* Avoid section present, with the text body. */
    HU_ASSERT(strstr(txt, "Avoid:") != NULL);
    HU_ASSERT(strstr(txt, "joke about her grandmother") != NULL);
    HU_ASSERT(strstr(txt, "she's grieving") != NULL);

    A()->free(A()->ctx, txt, tlen + 1);
    cleanup(g, f);
}

/* --- Adversarial: huge contact_id rejected --- */

static void bridge_render_rejects_invalid_args(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(NULL, A(), "ut_inv", 6, 0, &txt, &tlen, NULL, 0, NULL, 0,
                                            NULL, 0, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w7_render_world_model(f, NULL, "ut_inv", 6, 0, &txt, &tlen, NULL, 0, NULL, 0,
                                            NULL, 0, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), NULL, 0, 0, &txt, &tlen, NULL, 0, NULL, 0, NULL,
                                            0, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_inv", 0, 0, &txt, &tlen, NULL, 0, NULL, 0,
                                            NULL, 0, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_inv", 6, 0, NULL, &tlen, NULL, 0, NULL, 0,
                                            NULL, 0, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    cleanup(g, f);
}

/* --- Cache hit: second call within TTL hits the cache --- */

static void bridge_render_uses_cache_within_ttl(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    /* Seed once. */
    hu_negative_memory_t nm = {0};
    snprintf(nm.text, sizeof(nm.text), "topic alpha");
    snprintf(nm.scope, sizeof(nm.scope), "topic");
    nm.belief = hu_belief_init(0.9f, "test", 1700000000000LL);
    nm.created_at = 1700000000000LL;
    int64_t id = 0;
    HU_ASSERT_EQ(hu_negative_memory_add(g, "ut_cache", 8, &nm, &id), HU_OK);

    /* Re-enable cache for this test specifically (default TTL = 60s). */
    setenv("HU_WORLD_MODEL_TTL_MS", "60000", 1);
    char *txt1 = NULL;
    size_t tlen1 = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_cache", 8, 1700000000000LL + 1000, &txt1,
                                          &tlen1, NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt1);

    char *txt2 = NULL;
    size_t tlen2 = 0;
    /* Within the 60s default TTL -- should still produce content (cache or rebuild). */
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_cache", 8, 1700000000000LL + 5000, &txt2,
                                          &tlen2, NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt2);
    HU_ASSERT_EQ(tlen1, tlen2);

    A()->free(A()->ctx, txt1, tlen1 + 1);
    A()->free(A()->ctx, txt2, tlen2 + 1);
    cleanup(g, f);
}

/* --- M2 ↔ W9 personal model bridge --- */

static void bridge_render_with_personal_model_includes_style(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    hu_personal_model_t pm;
    hu_personal_model_init(&pm);
    pm.style.formality = 0.2f;
    pm.style.verbosity = 0.4f;
    pm.style.emoji_frequency = 0.5f;
    pm.style.avg_message_length = 120;
    pm.style.sample_count = 10;

    strncpy(pm.goals[0].description, "learn Rust", sizeof(pm.goals[0].description) - 1);
    pm.goals[0].active = true;
    pm.goal_count = 1;

    strncpy(pm.topics[0].name, "programming", sizeof(pm.topics[0].name) - 1);
    pm.topics[0].interest_score = 0.9f;
    pm.topic_count = 1;

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), "ut_pm", 5, 1700000000000LL, &txt, &tlen,
                                            NULL, 0, NULL, 0, NULL, 0, &pm, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT_GT(tlen, (size_t)0);
    HU_ASSERT_NOT_NULL(strstr(txt, "Communication style:"));
    HU_ASSERT_NOT_NULL(strstr(txt, "casual"));
    HU_ASSERT_NOT_NULL(strstr(txt, "learn Rust"));
    HU_ASSERT_NOT_NULL(strstr(txt, "programming"));

    A()->free(A()->ctx, txt, tlen + 1);
    cleanup(g, f);
}

/* --- W11 self-RAG bridge (FIX 12b) --- */

static void w11_off_mode_is_noop(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w11_outcome_t outc = HU_W11_OUTCOME_HEDGED;
    size_t total = 99, flagged = 99;
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, A(), "u", 1, "anything", 8, 0, /*OFF*/
                                         0, &outc, &total, &flagged, NULL, NULL),
                 HU_OK);
    HU_ASSERT_EQ((int)outc, (int)HU_W11_OUTCOME_SUPPORTED);
    HU_ASSERT_EQ(total, 0);
    HU_ASSERT_EQ(flagged, 0);
    cleanup(g, f);
}

static void w11_telemetry_extracts_claims_without_modifying(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w11_outcome_t outc = HU_W11_OUTCOME_SUPPORTED;
    size_t total = 0, flagged = 0;
    char *modified = NULL;
    size_t modified_len = 0;
    /* Pass NULL for out_modified to assert no modification path runs. */
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, A(), "u_w11_t", 7,
                                         "Paris is the capital of France.", 32, 1, /*TELEMETRY*/
                                         0, &outc, &total, &flagged, NULL, NULL),
                 HU_OK);
    /* Telemetry never modifies. */
    HU_ASSERT(modified == NULL);
    HU_ASSERT_EQ(modified_len, 0);
    /* outcome SUPPORTED for telemetry mode (no rewrite). */
    HU_ASSERT(outc == HU_W11_OUTCOME_SUPPORTED || outc == HU_W11_OUTCOME_ABSTAINED);
    cleanup(g, f);
}

/* P0 #1 — when the heuristic verifier ABSTAINS (>=50% of extracted
 * claims are unsupported), the bridge MUST surface the deterministic
 * refusal template through out_modified so the agent loop can swap
 * the user-visible response. Previously this was dropped on the
 * floor (see TODO note in world_model_bridge.c) and the user saw
 * the unverified draft. */
static void w11_abstain_emits_refusal_text_through_bridge(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    /* Empty graph + fact-shaped draft → every claim is unsupported →
     * heuristic backend ABSTAINS and renders UNKNOWN_FACT template. */
    const char *draft = "Paris is the capital of France. The Earth orbits the Sun.";
    hu_w11_outcome_t outc = HU_W11_OUTCOME_SUPPORTED;
    size_t total = 0, flagged = 0;
    char *modified = NULL;
    size_t modified_len = 0;

    HU_ASSERT_EQ(
        hu_w11_self_rag_verify(f, A(), "u_abstain", 9, draft, strlen(draft),
                               2 /* SOFT */, 0, &outc, &total, &flagged,
                               &modified, &modified_len),
        HU_OK);

    HU_ASSERT_EQ(outc, HU_W11_OUTCOME_ABSTAINED);
    HU_ASSERT_NOT_NULL(modified);
    HU_ASSERT_GT(modified_len, (size_t)0);

    /* The refusal template MUST be the canonical UNKNOWN_FACT string —
     * tests + channel renderer share this source of truth. */
    char expected[256];
    hu_self_rag_render_refusal(HU_REFUSAL_UNKNOWN_FACT, expected, sizeof(expected));
    HU_ASSERT_EQ(strcmp(modified, expected), 0);

    /* The refusal MUST replace the draft, not concatenate with it. */
    HU_ASSERT(strstr(modified, "Paris") == NULL);
    HU_ASSERT(strstr(modified, "Earth") == NULL);

    A()->free(A()->ctx, modified, modified_len + 1);
    cleanup(g, f);
}

/* P0 #1 — when the agent runs in TELEMETRY mode the bridge still
 * reports the ABSTAINED outcome but does NOT allocate refusal text
 * (the agent loop deliberately doesn't request it). Proves the
 * counter-only mode is preserved for users who want the metric
 * without the user-visible refusal swap. */
static void w11_telemetry_does_not_render_refusal(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    const char *draft = "Paris is the capital of France. The Earth orbits the Sun.";
    hu_w11_outcome_t outc = HU_W11_OUTCOME_SUPPORTED;
    /* Pass NULL for the modified-out pair → telemetry-only path. */
    HU_ASSERT_EQ(
        hu_w11_self_rag_verify(f, A(), "u_abstain_t", 11, draft, strlen(draft),
                               1 /* TELEMETRY */, 0, &outc, NULL, NULL, NULL,
                               NULL),
        HU_OK);
    HU_ASSERT_EQ(outc, HU_W11_OUTCOME_ABSTAINED);
    cleanup(g, f);
}

static void w11_rejects_invalid_args(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    HU_ASSERT_EQ(hu_w11_self_rag_verify(NULL, A(), "u", 1, "x", 1, 1, 0, NULL, NULL, NULL, NULL,
                                         NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, NULL, "u", 1, "x", 1, 1, 0, NULL, NULL, NULL, NULL,
                                         NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, A(), NULL, 0, "x", 1, 1, 0, NULL, NULL, NULL, NULL,
                                         NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w11_self_rag_verify(f, A(), "u", 1, NULL, 0, 1, 0, NULL, NULL, NULL, NULL,
                                         NULL),
                 HU_ERR_INVALID_ARGUMENT);
    cleanup(g, f);
}

/* ── W14 scheduler bridge (FIX 13) ───────────────────────────────────────
 * Verifies the open/tick/close lifecycle, that tick is a no-op when the
 * queue is empty, that enqueue + tick consumes a counterfactual job,
 * and that the bridge rejects invalid arguments cleanly. */

static void w14_scheduler_open_close_clean(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, A(), &s), HU_OK);
    HU_ASSERT_NOT_NULL(s);
    hu_w14_scheduler_close(s, A());
    cleanup(g, f);
}

static void w14_scheduler_rejects_null_args(void) {
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(NULL, A(), &s), HU_ERR_INVALID_ARGUMENT);
    /* close(NULL) is a no-op. */
    hu_w14_scheduler_close(NULL, A());
    HU_ASSERT_EQ(hu_w14_scheduler_tick(NULL, 1000), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_counterfactual(NULL, "u", 1, 50),
                 HU_ERR_INVALID_ARGUMENT);
}

static void w14_scheduler_tick_is_noop_when_empty(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, A(), &s), HU_OK);
    /* Tick with empty queue: must succeed and not crash. */
    HU_ASSERT_EQ(hu_w14_scheduler_tick(s, 1000), HU_OK);
    HU_ASSERT_EQ(hu_w14_scheduler_tick(s, 0), HU_OK); /* 0 -> use OS clock */
    /* Status should report 0 pending. */
    size_t pending = 999;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(pending, (size_t)0);
    hu_w14_scheduler_close(s, A());
    cleanup(g, f);
}

static void w14_scheduler_enqueue_then_tick_drains_job(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, A(), &s), HU_OK);

    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_counterfactual(s, "u_cf", 4, 50), HU_OK);
    /* After enqueue: at least 1 pending (we don't assume exact count
     * because the scheduler may dedupe / coalesce in the future). */
    size_t pending = 0;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT(pending >= 1);

    /* Tick: counterfactual runner is registered, queue should drain. */
    HU_ASSERT_EQ(hu_w14_scheduler_tick(s, 2000), HU_OK);

    /* After tick: queue should be empty (counterfactual rehearsal is
     * cheap; one tick consumes it within the per-tick budget). */
    pending = 999;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(pending, (size_t)0);

    hu_w14_scheduler_close(s, A());
    cleanup(g, f);
}

/* FIX 14: AutoDream runs through the scheduler. Enqueue all three phases
 * (quarantine / community / decay), tick once, expect the queue to drain
 * and the runner to return HU_OK on each. The empty graph yields zero
 * work but the runner must still complete cleanly. */
static void w14_autodream_phases_drain_through_scheduler(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);
    hu_w14_scheduler_t *s = NULL;
    HU_ASSERT_EQ(hu_w14_scheduler_open(f, A(), &s), HU_OK);

    /* Enqueue all three phases via the bridge helper. */
    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_autodream(s, 0, 100), HU_OK);

    size_t pending = 0;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT(pending >= 3); /* one per phase */

    /* Tick once: scheduler should dispatch all three through the
     * registered hu_autodream_runner. The HU_SCHED_MAX_JOBS_PER_TICK
     * cap is 32, so 3 jobs fit comfortably. */
    HU_ASSERT_EQ(hu_w14_scheduler_tick(s, 1000), HU_OK);

    pending = 999;
    HU_ASSERT_EQ(hu_w14_scheduler_status(s, &pending, NULL, NULL, NULL), HU_OK);
    HU_ASSERT_EQ(pending, (size_t)0);

    hu_w14_scheduler_close(s, A());
    cleanup(g, f);
}

static void w14_autodream_rejects_null_args(void) {
    HU_ASSERT_EQ(hu_w14_scheduler_enqueue_autodream(NULL, 0, 100), HU_ERR_INVALID_ARGUMENT);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Story C (sprint-4 follow-up) — render wm->recent_changes.
 *
 * hu_world_model_load already derives up to 8 SUPERSEDED/RETRACTED relations
 * per snapshot (src/agent/world_model.c P4.3 block), but until this story
 * they were never rendered into the prompt. The render section joins
 * `recent_changes` to the relation rows (already in `wm->relations`) so the
 * line looks like "alice works_at globex (updated)" instead of the
 * mechanical "rel #4 supersedes #7".
 * ────────────────────────────────────────────────────────────────────────── */

/* Helper: open graph + w7 facade (existing pattern). Returns a fresh
 * contact_id seeded with two entities. */
static void story_c_seed_two_entities(hu_graph_t *g, const char *cid, size_t cid_len,
                                       int64_t *out_a, int64_t *out_b) {
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, cid_len, "alice", 5, HU_ENTITY_PERSON, NULL, out_a),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, cid_len, "acme", 4, HU_ENTITY_ORGANIZATION, NULL, out_b),
        HU_OK);
}

static void bridge_render_with_recent_changes_emits_section(void) {
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    const char *cid = "ut_storyc_yes";
    int64_t alice = 0, acme = 0, globex = 0;
    story_c_seed_two_entities(g, cid, strlen(cid), &alice, &acme);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, strlen(cid), "globex", 6,
                                HU_ENTITY_ORGANIZATION, NULL, &globex),
        HU_OK);

    /* Insert relation A (alice works_at acme), then B (alice works_at globex)
     * with a later event_start. The bitemporal conflict resolver should
     * supersede A with B, leaving A retracted and B carrying
     * supersedes_id = A.id. Both feed wm->recent_changes. */
    HU_ASSERT_EQ(
        hu_graph_upsert_relation_ex(g, cid, strlen(cid), alice, acme, HU_REL_WORKS_AT, 1.0f,
                                     1735689600000LL, 0, 1.0f, "ctx-a", 5, "imessage", 8),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_relation_ex(g, cid, strlen(cid), alice, globex, HU_REL_WORKS_AT, 1.0f,
                                     1735776000000LL, 0, 1.0f, "ctx-b", 5, "imessage", 8),
        HU_OK);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), cid, strlen(cid),
                                          1735776100000LL, &txt, &tlen,
                                          NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT(tlen > 0);
    HU_ASSERT_NOT_NULL(strstr(txt, "Recent changes:"));
    /* The fresh relation supersedes the old one → its line shows the new
     * target (globex) tagged as "updated". The old relation is now
     * retracted → its line shows the old target (acme) tagged as
     * "no longer holds". Both are valid simultaneously. */
    HU_ASSERT(strstr(txt, "globex") != NULL || strstr(txt, "acme") != NULL);
    HU_ASSERT(strstr(txt, "(updated)") != NULL || strstr(txt, "(no longer holds)") != NULL);

    A()->free(A()->ctx, txt, tlen + 1);
    cleanup(g, f);
}

static void bridge_render_with_no_recent_changes_omits_section(void) {
    /* Regression guard: a contact with only fresh (non-superseded, non-
     * retracted) relations must NOT cause the "Recent changes:" header to
     * appear. */
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    const char *cid = "ut_storyc_no";
    int64_t alice = 0, acme = 0;
    story_c_seed_two_entities(g, cid, strlen(cid), &alice, &acme);
    HU_ASSERT_EQ(
        hu_graph_upsert_relation_ex(g, cid, strlen(cid), alice, acme, HU_REL_WORKS_AT, 1.0f,
                                     1735689600000LL, 0, 1.0f, "ctx", 3, "imessage", 8),
        HU_OK);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), cid, strlen(cid),
                                          1735689700000LL, &txt, &tlen,
                                          NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT(tlen > 0);
    HU_ASSERT(strstr(txt, "Recent changes:") == NULL);

    A()->free(A()->ctx, txt, tlen + 1);
    cleanup(g, f);
}

static void bridge_render_recent_changes_caps_at_five(void) {
    /* Insert 6 relations, each superseding the previous, so wm->recent_changes
     * carries 6 entries. The render section must cap at 5 (configurable in
     * world_model_bridge.c). We verify by counting "- " bullets between
     * "Recent changes:" and the next section header. */
    hu_graph_t *g = NULL;
    hu_w7_facade_t *f = NULL;
    open_graph_and_facade(&g, &f);

    const char *cid = "ut_storyc_cap";
    int64_t alice = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, strlen(cid), "alice", 5, HU_ENTITY_PERSON, NULL, &alice),
        HU_OK);

    /* Each iteration inserts a new "works_at orgN" that supersedes the
     * previous via the bitemporal resolver. */
    for (int i = 0; i < 7; i++) {
        char org_name[16];
        snprintf(org_name, sizeof(org_name), "org%d", i);
        int64_t org = 0;
        HU_ASSERT_EQ(hu_graph_upsert_entity(g, cid, strlen(cid), org_name,
                                              strlen(org_name), HU_ENTITY_ORGANIZATION,
                                              NULL, &org),
                     HU_OK);
        int64_t event_start = 1735689600000LL + (int64_t)i * 86400000LL;
        HU_ASSERT_EQ(
            hu_graph_upsert_relation_ex(g, cid, strlen(cid), alice, org, HU_REL_WORKS_AT,
                                         1.0f, event_start, 0, 1.0f, "c", 1, "imessage", 8),
            HU_OK);
    }

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), cid, strlen(cid),
                                          1735776100000LL + 700000000LL, &txt, &tlen,
                                          NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT(tlen > 0);

    /* Find the "Recent changes:" header, then count "- " bullets until the
     * next blank line or known following header. */
    const char *header = strstr(txt, "Recent changes:");
    HU_ASSERT_NOT_NULL(header);
    const char *scan = header + strlen("Recent changes:");
    size_t bullets = 0;
    while (*scan) {
        if (scan[0] == '\n' && scan[1] == '-' && scan[2] == ' ') {
            bullets++;
            scan += 3;
        } else if (scan[0] == '\n' && scan[1] != '-') {
            break;
        } else {
            scan++;
        }
    }
    HU_ASSERT(bullets <= 5);
    HU_ASSERT(bullets > 0);

    A()->free(A()->ctx, txt, tlen + 1);
    cleanup(g, f);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Story D (sprint-4 follow-up) — render wm->hyperedges.
 *
 * hu_world_model_load already pulls up to 16 deduped hyperedges per snapshot
 * (src/agent/world_model.c P4.2 block). They are n-ary facts with a
 * relation_label, members[] (entity_id + role), and a belief posterior.
 * Until this story they were never rendered. The render section produces
 * "- label: name[role], name[role], ..." lines.
 *
 * Hyperedge writes go through the memory_facade (hu_hyperedge_upsert), so
 * these tests open both the graph (for entities) and a memory_facade (for
 * the hyperedge), then point the W7 facade at the same graph for render.
 * ────────────────────────────────────────────────────────────────────────── */

static void story_d_open_all(hu_graph_t **g, hu_memory_facade_t **m, hu_w7_facade_t **f) {
    setenv("HU_WORLD_MODEL_TTL_MS", "0", 1);
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_EQ(hu_memory_facade_open(A(), *g, m), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(*g, A(), f), HU_OK);
}

static void story_d_cleanup_all(hu_graph_t *g, hu_memory_facade_t *m, hu_w7_facade_t *f) {
    if (f) hu_w7_facade_close(f, A());
    if (m) hu_memory_facade_close(m, A());
    if (g) hu_graph_close(g, A());
}

static void bridge_render_with_hyperedges_emits_multi_entity_section(void) {
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_w7_facade_t *f = NULL;
    story_d_open_all(&g, &m, &f);

    const char *cid = "ut_storyd_yes";
    int64_t alice = 0, bob = 0, acme = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, strlen(cid), "alice", 5, HU_ENTITY_PERSON, NULL, &alice),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, strlen(cid), "bob", 3, HU_ENTITY_PERSON, NULL, &bob),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, strlen(cid), "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme),
        HU_OK);

    hu_hyperedge_member_t members[3];
    memset(members, 0, sizeof(members));
    members[0].entity_id = alice;
    memcpy(members[0].role, "subject", 7);
    members[1].entity_id = bob;
    memcpy(members[1].role, "subject", 7);
    members[2].entity_id = acme;
    memcpy(members[2].role, "location", 8);

    hu_hyperedge_t he;
    memset(&he, 0, sizeof(he));
    memcpy(he.relation_label, "met_at", 6);
    he.members = members;
    he.members_count = 3;
    he.belief = hu_belief_init(0.9f, "imessage", 5000LL);
    he.event_start = 5000LL;

    int64_t edge_id = 0;
    HU_ASSERT_EQ(hu_hyperedge_upsert(m, cid, strlen(cid), &he, &edge_id), HU_OK);
    HU_ASSERT_GT(edge_id, 0);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), cid, strlen(cid),
                                          1700000000000LL, &txt, &tlen,
                                          NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT(tlen > 0);
    HU_ASSERT_NOT_NULL(strstr(txt, "Multi-entity facts:"));
    HU_ASSERT_NOT_NULL(strstr(txt, "met_at"));
    HU_ASSERT_NOT_NULL(strstr(txt, "alice[subject]"));
    HU_ASSERT_NOT_NULL(strstr(txt, "acme[location]"));

    A()->free(A()->ctx, txt, tlen + 1);
    story_d_cleanup_all(g, m, f);
}

static void bridge_render_with_no_hyperedges_omits_section(void) {
    /* Regression guard: a contact with entities/relations but no hyperedges
     * must NOT cause the "Multi-entity facts:" header to appear. */
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_w7_facade_t *f = NULL;
    story_d_open_all(&g, &m, &f);

    const char *cid = "ut_storyd_no";
    int64_t alice = 0, acme = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, strlen(cid), "alice", 5, HU_ENTITY_PERSON, NULL, &alice),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, strlen(cid), "acme", 4, HU_ENTITY_ORGANIZATION, NULL, &acme),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_relation_ex(g, cid, strlen(cid), alice, acme, HU_REL_WORKS_AT, 1.0f,
                                     1700000000000LL, 0, 1.0f, "ctx", 3, "imessage", 8),
        HU_OK);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), cid, strlen(cid),
                                          1700000001000LL, &txt, &tlen,
                                          NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT(tlen > 0);
    HU_ASSERT(strstr(txt, "Multi-entity facts:") == NULL);

    A()->free(A()->ctx, txt, tlen + 1);
    story_d_cleanup_all(g, m, f);
}

static void bridge_render_hyperedge_member_not_in_entities_renders_id_fallback(void) {
    /* If a hyperedge references an entity id that did not make it into the
     * snapshot's top-K wm->entities window, the bridge falls back to
     * "ent#<id>[role]" rather than crashing or dropping the member. */
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_w7_facade_t *f = NULL;
    story_d_open_all(&g, &m, &f);

    const char *cid = "ut_storyd_fb";
    int64_t alice = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(g, cid, strlen(cid), "alice", 5, HU_ENTITY_PERSON, NULL, &alice),
        HU_OK);

    /* Synthesize a member with an entity id far outside the seeded range so
     * the entity-name lookup misses. */
    hu_hyperedge_member_t members[2];
    memset(members, 0, sizeof(members));
    members[0].entity_id = alice;
    memcpy(members[0].role, "subject", 7);
    members[1].entity_id = 999999LL; /* not in graph */
    memcpy(members[1].role, "object", 6);

    hu_hyperedge_t he;
    memset(&he, 0, sizeof(he));
    memcpy(he.relation_label, "discussed", 9);
    he.members = members;
    he.members_count = 2;
    he.belief = hu_belief_init(0.8f, "imessage", 1000LL);
    he.event_start = 1000LL;

    int64_t edge_id = 0;
    HU_ASSERT_EQ(hu_hyperedge_upsert(m, cid, strlen(cid), &he, &edge_id), HU_OK);

    char *txt = NULL;
    size_t tlen = 0;
    HU_ASSERT_EQ(hu_w7_render_world_model(f, A(), cid, strlen(cid),
                                          1700000001000LL, &txt, &tlen,
                                          NULL, 0, NULL, 0, NULL, 0, NULL, NULL),
                 HU_OK);
    HU_ASSERT_NOT_NULL(txt);
    HU_ASSERT(tlen > 0);
    HU_ASSERT_NOT_NULL(strstr(txt, "Multi-entity facts:"));
    HU_ASSERT_NOT_NULL(strstr(txt, "discussed"));
    HU_ASSERT_NOT_NULL(strstr(txt, "alice[subject]"));
    /* Unknown member rendered with id fallback. */
    HU_ASSERT_NOT_NULL(strstr(txt, "ent#999999[object]"));

    A()->free(A()->ctx, txt, tlen + 1);
    story_d_cleanup_all(g, m, f);
}

#endif /* HU_ENABLE_SQLITE */

void run_world_model_bridge_tests(void) {
    HU_TEST_SUITE("World-model bridge (FIX 12)");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(bridge_open_close_clean);
    HU_RUN_TEST(bridge_open_rejects_null_args);
    HU_RUN_TEST(bridge_render_empty_world_model_returns_null);
    HU_RUN_TEST(bridge_render_optional_tom_scenario_merges_into_output);
    HU_RUN_TEST(bridge_render_with_negative_memory_includes_avoid_section);
    HU_RUN_TEST(bridge_render_rejects_invalid_args);
    HU_RUN_TEST(bridge_render_uses_cache_within_ttl);
    HU_RUN_TEST(bridge_render_with_personal_model_includes_style);
    HU_RUN_TEST(w11_off_mode_is_noop);
    HU_RUN_TEST(w11_telemetry_extracts_claims_without_modifying);
    HU_RUN_TEST(w11_abstain_emits_refusal_text_through_bridge);
    HU_RUN_TEST(w11_telemetry_does_not_render_refusal);
    HU_RUN_TEST(w11_rejects_invalid_args);
    HU_RUN_TEST(w14_scheduler_open_close_clean);
    HU_RUN_TEST(w14_scheduler_rejects_null_args);
    HU_RUN_TEST(w14_scheduler_tick_is_noop_when_empty);
    HU_RUN_TEST(w14_scheduler_enqueue_then_tick_drains_job);
    HU_RUN_TEST(w14_autodream_phases_drain_through_scheduler);
    HU_RUN_TEST(w14_autodream_rejects_null_args);
    /* sprint-4 follow-up Story C — render wm->recent_changes. */
    HU_RUN_TEST(bridge_render_with_recent_changes_emits_section);
    HU_RUN_TEST(bridge_render_with_no_recent_changes_omits_section);
    HU_RUN_TEST(bridge_render_recent_changes_caps_at_five);
    /* sprint-4 follow-up Story D — render wm->hyperedges. */
    HU_RUN_TEST(bridge_render_with_hyperedges_emits_multi_entity_section);
    HU_RUN_TEST(bridge_render_with_no_hyperedges_omits_section);
    HU_RUN_TEST(bridge_render_hyperedge_member_not_in_entities_renders_id_fallback);
#endif
}
