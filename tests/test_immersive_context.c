/* src/agent/turn/immersive_context.c — HU_IMMERSIVE_CONTEXT.
 *
 * The immersive branch of hu_prompt_build_system (the prompt production sends)
 * returned before commitments, episodic replay, emotional state, presence,
 * proactive/superhuman insight, goals and residue were appended. These tests
 * pin the composer that brings the most useful of them back under a 1.5 KB
 * budget — priority order, whole-item truncation, empty → nothing — and the
 * gate: OFF/SHADOW leave the prompt byte-identical, LIVE adds exactly the
 * block (stripping it restores the OFF prompt byte for byte). */
#include "human/agent/immersive_context.h"
#include "human/agent/prompt.h"
#include "human/core/allocator.h"
#include "human/providers/private_context.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_commit[] = "### Active Commitments\n\n"
                               "- call my sister tomorrow (by user, 2026-10-01)\n";
static const char k_episodic[] =
    "### Cognitive Replay\n\n"
    "*Prior approaches that worked for similar problems (use as hints, not facts):*\n\n"
    "- **comfort** (quality: 80%, seen 3 times): ask before advising\n"
    "- **planning** (quality: 70%, seen 2 times): SECOND_EPISODE_MARKER\n\n";
static const char k_emotional[] =
    "## Emotional Context\n\n"
    "- Dominant emotion: **tired** (valence: -0.20, intensity: 0.40)\n"
    "- Secondary: worried\n"
    "- Confidence: 70%\n";
static const char k_presence[] = "[PRESENCE: Light mode. Brief, breezy. Don't over-invest.]";
static const char k_proactive[] =
    "### Proactive Awareness\n\n- PROACTIVE_MARKER ask about the trip\n";
static const char k_superhuman[] = "### Superhuman Insights\n\n#### Commitment Keeper\n"
                                   "SUPERHUMAN_MARKER follow up on the sister call.\n\n";
static const char k_goals[] = "[CONVERSATION GOALS with this contact]:\n"
                              "1. [HIGH] GOALS_MARKER hear how the move went (0/3 attempts)\n"
                              "   Success signal: they mention the new place\n";
static const char k_residue[] = "RESIDUE_MARKER this person was vulnerable with you recently.";

static hu_prompt_config_t full_cfg(void) {
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.commitment_context = k_commit;
    cfg.commitment_context_len = sizeof(k_commit) - 1;
    cfg.episodic_replay = k_episodic;
    cfg.episodic_replay_len = sizeof(k_episodic) - 1;
    cfg.emotional_context = k_emotional;
    cfg.emotional_context_len = sizeof(k_emotional) - 1;
    cfg.presence_context = k_presence;
    cfg.presence_context_len = sizeof(k_presence) - 1;
    cfg.proactive_context = k_proactive;
    cfg.proactive_context_len = sizeof(k_proactive) - 1;
    cfg.superhuman_context = k_superhuman;
    cfg.superhuman_context_len = sizeof(k_superhuman) - 1;
    cfg.conv_goals_context = k_goals;
    cfg.conv_goals_context_len = sizeof(k_goals) - 1;
    cfg.residue_carryover = k_residue;
    cfg.residue_carryover_len = sizeof(k_residue) - 1;
    return cfg;
}

static size_t pos_of(const char *hay, const char *needle) {
    const char *p = strstr(hay, needle);
    return p ? (size_t)(p - hay) : (size_t)-1;
}

static void immersive_context_empty_input_composes_nothing(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char *out = (char *)&cfg; /* non-NULL sentinel: compose must reset it */
    size_t out_len = 99;
    hu_immersive_context_stats_t st = {.fields_used = 7, .bytes = 7};
    HU_ASSERT_EQ(hu_immersive_context_compose(&a, &cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &out,
                                              &out_len, &st),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);
    HU_ASSERT_EQ(st.fields_used, 0);
    HU_ASSERT_EQ(st.bytes, 0);
    HU_ASSERT_FALSE(st.truncated);
}

/* Builder chrome only (headings, a placeholder, a bare tag): no item. */
static void immersive_context_chrome_only_composes_nothing(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    static const char goals[] = "[No active conversation goals with this contact]";
    static const char commit[] = "### Active Commitments\n\n";
    static const char presence[] = "[NARRATIVE SELF:]";
    cfg.conv_goals_context = goals;
    cfg.conv_goals_context_len = sizeof(goals) - 1;
    cfg.commitment_context = commit;
    cfg.commitment_context_len = sizeof(commit) - 1;
    cfg.presence_context = presence;
    cfg.presence_context_len = sizeof(presence) - 1;
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_immersive_context_compose(&a, &cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &out,
                                              &out_len, NULL),
                 HU_OK);
    HU_ASSERT_NULL(out);
}

static void immersive_context_orders_items_by_priority(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_prompt_config_t cfg = full_cfg();
    char *out = NULL;
    size_t out_len = 0;
    hu_immersive_context_stats_t st;
    HU_ASSERT_EQ(hu_immersive_context_compose(&a, &cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &out,
                                              &out_len, &st),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(out_len, strlen(out));
    HU_ASSERT_EQ(st.bytes, out_len);
    HU_ASSERT_EQ(st.fields_used, 8);
    HU_ASSERT_EQ(st.field_mask, 0xffu);
    HU_ASSERT_FALSE(st.truncated);
    HU_ASSERT_EQ(pos_of(out, HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT), 0);
    size_t p_commit = pos_of(out, "- call my sister tomorrow (by user, 2026-10-01)\n");
    size_t p_epi = pos_of(out, "- comfort (quality: 80%, seen 3 times): ask before advising\n");
    size_t p_emo = pos_of(out, "- Dominant emotion: tired");
    size_t p_pres = pos_of(out, "- Light mode. Brief, breezy. Don't over-invest.\n");
    size_t p_pro = pos_of(out, "PROACTIVE_MARKER");
    size_t p_sup = pos_of(out, "SUPERHUMAN_MARKER");
    size_t p_goal = pos_of(out, "- GOALS_MARKER hear how the move went (0/3 attempts) Success "
                                "signal: they mention the new place\n");
    size_t p_res = pos_of(out, "RESIDUE_MARKER");
    HU_ASSERT_TRUE(p_commit != (size_t)-1 && p_epi != (size_t)-1 && p_emo != (size_t)-1);
    HU_ASSERT_TRUE(p_pres != (size_t)-1 && p_pro != (size_t)-1 && p_sup != (size_t)-1);
    HU_ASSERT_TRUE(p_goal != (size_t)-1 && p_res != (size_t)-1);
    HU_ASSERT_TRUE(p_commit < p_epi && p_epi < p_emo && p_emo < p_pres && p_pres < p_pro);
    HU_ASSERT_TRUE(p_pro < p_sup && p_sup < p_goal && p_goal < p_res);
    /* Builder chrome is gone; the per-field caps hold (episodic 1, emotional 2). */
    HU_ASSERT_NULL(strstr(out, "###"));
    HU_ASSERT_NULL(strstr(out, "**"));
    HU_ASSERT_NULL(strstr(out, "[PRESENCE"));
    HU_ASSERT_NULL(strstr(out, "SECOND_EPISODE_MARKER"));
    HU_ASSERT_NULL(strstr(out, "Confidence: 70%"));
    HU_ASSERT_NOT_NULL(strstr(out, "- Secondary: worried\n"));
    /* Strippable: one blank line, at the very end. */
    HU_ASSERT_EQ(pos_of(out, "\n\n"), out_len - 2);
    a.free(a.ctx, out, out_len + 1);
}

/* An item that cannot fit is dropped WHOLE; a later, smaller item still fits. */
static void immersive_context_budget_drops_whole_items(void) {
    hu_allocator_t a = hu_system_allocator();
    char commits[3 * 700];
    size_t n = 0;
    for (int i = 0; i < 3; i++) {
        n += (size_t)snprintf(commits + n, sizeof(commits) - n, "- COMMIT%d ", i);
        memset(commits + n, 'a' + i, 600);
        n += 600;
        commits[n++] = '.';
        commits[n++] = '\n';
    }
    commits[n] = '\0';
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.commitment_context = commits;
    cfg.commitment_context_len = n;
    cfg.presence_context = k_presence;
    cfg.presence_context_len = sizeof(k_presence) - 1;
    char *out = NULL;
    size_t out_len = 0;
    hu_immersive_context_stats_t st;
    HU_ASSERT_EQ(hu_immersive_context_compose(&a, &cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &out,
                                              &out_len, &st),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len <= HU_IMMERSIVE_CONTEXT_BUDGET_BYTES);
    HU_ASSERT_TRUE(st.truncated);
    HU_ASSERT_NOT_NULL(strstr(out, "COMMIT0"));
    HU_ASSERT_NOT_NULL(strstr(out, "COMMIT1"));
    HU_ASSERT_NULL(strstr(out, "COMMIT2")); /* dropped whole ... */
    HU_ASSERT_NULL(strstr(out, "ccc"));     /* ... with none of its body */
    char full[640];
    memset(full, 'b', 600);
    full[600] = '.';
    full[601] = '\0';
    HU_ASSERT_NOT_NULL(strstr(out, full)); /* kept items are complete */
    HU_ASSERT_NOT_NULL(strstr(out, "- Light mode. Brief, breezy. Don't over-invest.\n"));
    HU_ASSERT_EQ(st.field_mask,
                 (unsigned)(HU_IMMERSIVE_CTX_COMMITMENT | HU_IMMERSIVE_CTX_PRESENCE));
    a.free(a.ctx, out, out_len + 1);
}

/* A budget too small for any item composes nothing — never a partial line. */
static void immersive_context_tiny_budget_composes_nothing(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_prompt_config_t cfg = full_cfg();
    char *out = NULL;
    size_t out_len = 0;
    hu_immersive_context_stats_t st;
    HU_ASSERT_EQ(hu_immersive_context_compose(&a, &cfg, 40, &out, &out_len, &st), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_TRUE(st.truncated);
}

/* ── gate: through the real prompt builder ─────────────────────────────── */

typedef struct gate_env {
    char *saved;
    bool had;
} gate_env_t;

static void gate_set(gate_env_t *g, const char *value) {
    const char *v = getenv("HU_IMMERSIVE_CONTEXT");
    g->had = v != NULL;
    g->saved = v ? strdup(v) : NULL;
    if (value)
        setenv("HU_IMMERSIVE_CONTEXT", value, 1);
    else
        unsetenv("HU_IMMERSIVE_CONTEXT");
}

static void gate_restore(gate_env_t *g) {
    if (g->had)
        setenv("HU_IMMERSIVE_CONTEXT", g->saved, 1);
    else
        unsetenv("HU_IMMERSIVE_CONTEXT");
    free(g->saved);
}

/* Removes the wall-clock "Right now it is ..." line so two builds a minute
 * apart compare equal. */
static void drop_clock_line(char *s) {
    char *p = strstr(s, "\nRight now it is ");
    if (!p)
        return;
    char *e = strchr(p + 1, '\n');
    if (e)
        memmove(p, e, strlen(e) + 1);
}

static char *build_immersive(const char *gate, size_t *len) {
    hu_allocator_t a = hu_system_allocator();
    hu_prompt_config_t cfg = full_cfg();
    cfg.persona_immersive = true;
    cfg.persona_prompt = "PERSONA_HEAD you text like yourself.";
    cfg.persona_prompt_len = strlen(cfg.persona_prompt);
    cfg.memory_context = "MEMORY_MARKER she moved to Tampa";
    cfg.memory_context_len = strlen(cfg.memory_context);
    cfg.suppress_prompt_budget_diagnostic = true;
    gate_env_t g;
    gate_set(&g, gate);
    char *out = NULL;
    hu_error_t err = hu_prompt_build_system(&a, &cfg, NULL, NULL, &out, len);
    gate_restore(&g);
    if (err != HU_OK)
        return NULL;
    drop_clock_line(out);
    *len = strlen(out);
    return out;
}

static void free_prompt(char *p, size_t len) {
    (void)len;
    hu_allocator_t a = hu_system_allocator();
    /* drop_clock_line shortened the string in place; the allocation size is
     * not tracked by the system allocator, so the original size is moot. */
    a.free(a.ctx, p, strlen(p) + 1);
}

static void immersive_context_gate_off_and_shadow_keep_prompt_identical(void) {
    size_t l_unset = 0, l_off = 0, l_shadow = 0;
    char *unset = build_immersive(NULL, &l_unset);
    char *off = build_immersive("off", &l_off);
    char *shadow = build_immersive("shadow", &l_shadow);
    HU_ASSERT_NOT_NULL(unset);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_NOT_NULL(shadow);
    HU_ASSERT_STR_EQ(off, unset);
    HU_ASSERT_STR_EQ(shadow, unset);
    HU_ASSERT_NULL(strstr(unset, HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT));
    HU_ASSERT_NULL(strstr(unset, "call my sister tomorrow")); /* today's drop */
    free_prompt(unset, l_unset);
    free_prompt(off, l_off);
    free_prompt(shadow, l_shadow);
}

/* Headline: pre (OFF) lacks the commitment; LIVE carries it inside the block;
 * removing the block restores the OFF prompt byte for byte. */
static void immersive_context_gate_live_adds_exactly_the_block(void) {
    size_t l_off = 0, l_live = 0;
    char *off = build_immersive(NULL, &l_off);
    char *live = build_immersive("live", &l_live);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_NOT_NULL(live);
    HU_ASSERT_NULL(strstr(off, "call my sister tomorrow"));
    const char *block = strstr(live, HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT);
    HU_ASSERT_NOT_NULL(block);
    HU_ASSERT_NOT_NULL(strstr(block, "- call my sister tomorrow (by user, 2026-10-01)\n"));
    /* Ahead of the trimmable middle: the block precedes memory context. */
    HU_ASSERT_TRUE(block < strstr(live, "MEMORY_MARKER"));
    HU_ASSERT_TRUE(l_live > l_off);
    size_t stripped = hu_private_context_strip(live, l_live);
    HU_ASSERT_EQ(stripped, l_off);
    HU_ASSERT_STR_EQ(live, off);
    free_prompt(off, l_off);
    free_prompt(live, l_live);
}

void run_immersive_context_tests(void) {
    HU_TEST_SUITE("ImmersiveContext");
    HU_RUN_TEST(immersive_context_empty_input_composes_nothing);
    HU_RUN_TEST(immersive_context_chrome_only_composes_nothing);
    HU_RUN_TEST(immersive_context_orders_items_by_priority);
    HU_RUN_TEST(immersive_context_budget_drops_whole_items);
    HU_RUN_TEST(immersive_context_tiny_budget_composes_nothing);
    HU_RUN_TEST(immersive_context_gate_off_and_shadow_keep_prompt_identical);
    HU_RUN_TEST(immersive_context_gate_live_adds_exactly_the_block);
}
