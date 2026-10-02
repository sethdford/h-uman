/* src/agent/turn/immersive_context.c — HU_IMMERSIVE_CONTEXT.
 *
 * The immersive branch of hu_prompt_build_system (the prompt production sends)
 * returned before commitments, emotional state, proactive/superhuman insight,
 * goals and residue were appended. These tests pin the composer that brings
 * the FACTS about the person back under a 1.5 KB budget: priority order,
 * directive sentences dropped ("Generate a natural follow-up", "You are
 * tracking N commitments", "Light mode. Brief, breezy."), duplicates of Core
 * Memory or an earlier item dropped, whole-item truncation, empty → nothing —
 * and the gate: OFF/SHADOW leave the prompt byte-identical, LIVE adds exactly
 * the block, and a turn that is not local composes nothing. The builder text
 * below is the builders' real output format. */
#include "human/agent/immersive_context.h"
#include "human/agent/prompt.h"
#include "human/core/allocator.h"
#include "human/providers/private_context.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_commit[] = "### Active Commitments\n\n"
                               "- call my sister tomorrow (by user, 2026-10-01)\n"
                               "- bring the ladder back (by assistant, 2026-09-30)\n";
static const char k_episodic[] =
    "### Cognitive Replay\n\n"
    "*Prior approaches that worked for similar problems (use as hints, not facts):*\n\n"
    "- **comfort** (quality: 80%, seen 3 times): EPISODE_MARKER ask before advising\n";
static const char k_emotional[] =
    "## Emotional Context\n\n"
    "- Dominant emotion: **tired** (valence: -0.20, intensity: 0.40)\n"
    "- Confidence: 70%\n"
    "- Secondary: worried\n"
    "Match their energy but stay calm.\n";
static const char k_presence[] = "[PRESENCE: Light mode. Brief, breezy. Don't over-invest.]";
static const char k_proactive[] =
    "### Proactive Awareness\n\n"
    "- MORNING CONTEXT: Generate a morning message that references what's relevant today.\n"
    "- COMMITMENT FOLLOW-UP: They mentioned 'call my sister tomorrow' (created 2026-10-01). "
    "Generate a natural follow-up that shows genuine interest.\n"
    "- CHECK-IN: Consider how the user is feeling.\n"
    "- Active commitments: call my sister tomorrow.\n"
    "- MILESTONE: They started the new job at the marina this week.\n";
static const char k_superhuman[] =
    "### Superhuman Insights\n\n#### Commitment Keeper\n"
    "You are tracking 1 active commitments for the user. Follow up naturally on any that are "
    "overdue.\n\n";
static const char k_goals[] = "[CONVERSATION GOALS with this contact]:\n"
                              "1. [HIGH] hear how the move went (0/3 attempts)\n"
                              "   Success signal: they mention the new place\n";
static const char k_residue[] =
    "This person was vulnerable with you recently (12 hours ago). Start warmer than usual. "
    "They took a risk by opening up \xE2\x80\x94 honor that by being a little gentler.";

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

static char *compose(const hu_prompt_config_t *cfg, size_t budget, size_t *len,
                     hu_immersive_context_stats_t *st) {
    hu_allocator_t a = hu_system_allocator();
    char *out = NULL;
    *len = 0;
    if (hu_immersive_context_compose(&a, cfg, budget, &out, len, st) != HU_OK)
        return NULL;
    return out;
}

static void free_block(char *out, size_t len) {
    hu_allocator_t a = hu_system_allocator();
    if (out)
        a.free(a.ctx, out, len + 1);
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

/* Builder chrome or directives only: no fact, so no block. */
static void immersive_context_chrome_and_directives_compose_nothing(void) {
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    static const char goals[] = "[No active conversation goals with this contact]";
    static const char commit[] = "### Active Commitments\n\n";
    cfg.conv_goals_context = goals;
    cfg.conv_goals_context_len = sizeof(goals) - 1;
    cfg.commitment_context = commit;
    cfg.commitment_context_len = sizeof(commit) - 1;
    cfg.superhuman_context = k_superhuman;
    cfg.superhuman_context_len = sizeof(k_superhuman) - 1;
    cfg.presence_context = k_presence; /* not a source */
    cfg.presence_context_len = sizeof(k_presence) - 1;
    size_t len = 0;
    hu_immersive_context_stats_t st;
    char *out = compose(&cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &len, &st);
    HU_ASSERT_NULL(out);
    HU_ASSERT_GT(st.directives_dropped, 0);
}

static void immersive_context_keeps_facts_in_priority_order(void) {
    hu_prompt_config_t cfg = full_cfg();
    size_t len = 0;
    hu_immersive_context_stats_t st;
    char *out = compose(&cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(len, strlen(out));
    HU_ASSERT_EQ(st.bytes, len);
    HU_ASSERT_FALSE(st.truncated);
    HU_ASSERT_EQ(pos_of(out, HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT), 0);
    size_t p_commit = pos_of(out, "- call my sister tomorrow (their words)\n");
    size_t p_ladder = pos_of(out, "- bring the ladder back (your words)\n");
    size_t p_emo = pos_of(out, "- Dominant emotion: tired\n");
    size_t p_pro = pos_of(out, "- They started the new job at the marina this week.\n");
    size_t p_goal = pos_of(out, "- hear how the move went\n");
    size_t p_res = pos_of(out, "- This person was vulnerable with you recently. They took a risk "
                               "by opening up.\n");
    HU_ASSERT_TRUE(p_commit != (size_t)-1 && p_ladder != (size_t)-1 && p_emo != (size_t)-1);
    HU_ASSERT_TRUE(p_pro != (size_t)-1 && p_goal != (size_t)-1 && p_res != (size_t)-1);
    HU_ASSERT_TRUE(p_commit < p_ladder && p_ladder < p_emo && p_emo < p_pro);
    HU_ASSERT_TRUE(p_pro < p_goal && p_goal < p_res);
    HU_ASSERT_NOT_NULL(strstr(out, "- Secondary: worried\n"));
    HU_ASSERT_EQ(st.field_mask, (unsigned)(HU_IMMERSIVE_CTX_COMMITMENT |
                                           HU_IMMERSIVE_CTX_EMOTIONAL | HU_IMMERSIVE_CTX_PROACTIVE |
                                           HU_IMMERSIVE_CTX_CONV_GOALS | HU_IMMERSIVE_CTX_RESIDUE));
    /* Instructions to the model and builder chrome never pass through. */
    static const char *const k_absent[] = {
        "Generate",
        "Consider",
        "You are tracking",
        "Follow up",
        "the user",
        "Start warmer",
        "honor that",
        "Match their",
        "Confidence",
        "Light mode",
        "EPISODE_MARKER",
        "Success signal",
        "###",
        "**",
        "(by user",
        "valence",
        "0/3",
        "They mentioned",
    };
    for (size_t i = 0; i < sizeof(k_absent) / sizeof(k_absent[0]); i++) {
        if (strstr(out, k_absent[i]))
            printf("    block still carries \"%s\"\n", k_absent[i]);
        HU_ASSERT_NULL(strstr(out, k_absent[i]));
    }
    /* The proactive follow-up repeats the commitment: dropped as a duplicate. */
    HU_ASSERT_GT(st.duplicates_dropped, 0);
    HU_ASSERT_GT(st.directives_dropped, 0);
    HU_ASSERT_EQ(pos_of(out, "\n\n"), len - 2); /* one blank line, at the end */
    free_block(out, len);
}

/* A fact Core Memory already carries is not repeated in the block. */
static void immersive_context_drops_what_core_memory_already_says(void) {
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.commitment_context = k_commit;
    cfg.commitment_context_len = sizeof(k_commit) - 1;
    size_t len = 0;
    hu_immersive_context_stats_t st;
    char *pre = compose(&cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &len, &st);
    HU_ASSERT_NOT_NULL(pre); /* precondition: without memory, both are kept */
    HU_ASSERT_NOT_NULL(strstr(pre, "call my sister tomorrow"));
    free_block(pre, len);
    static const char mem[] = "### Memory: commitment:abc\n"
                              "{\"statement\":\"I will call my sister tomorrow\","
                              "\"summary\":\"call my sister tomorrow\"}\n";
    cfg.memory_context = mem;
    cfg.memory_context_len = sizeof(mem) - 1;
    char *out = compose(&cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NULL(strstr(out, "sister"));
    HU_ASSERT_NOT_NULL(strstr(out, "bring the ladder back"));
    HU_ASSERT_EQ(st.duplicates_dropped, 1);
    free_block(out, len);
}

/* An item that cannot fit is dropped WHOLE; a later, smaller item still fits. */
static void immersive_context_budget_drops_whole_items(void) {
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
    cfg.residue_carryover = k_residue;
    cfg.residue_carryover_len = sizeof(k_residue) - 1;
    size_t len = 0;
    hu_immersive_context_stats_t st;
    char *out = compose(&cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(len <= HU_IMMERSIVE_CONTEXT_BUDGET_BYTES);
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
    HU_ASSERT_NOT_NULL(strstr(out, "vulnerable with you recently"));
    HU_ASSERT_EQ(st.field_mask, (unsigned)(HU_IMMERSIVE_CTX_COMMITMENT | HU_IMMERSIVE_CTX_RESIDUE));
    free_block(out, len);
}

/* An oversize first line consumes its indented continuation lines: they never
 * surface as items of their own. */
static void immersive_context_oversize_item_swallows_its_continuations(void) {
    static char goals[4200];
    size_t n = (size_t)snprintf(goals, sizeof(goals), "1. [HIGH] ");
    memset(goals + n, 'x', 3500);
    n += 3500;
    n += (size_t)snprintf(goals + n, sizeof(goals) - n,
                          "\n   Success signal: CONT_MARKER they mention it\n"
                          "2. [LOW] NEXT_GOAL_MARKER ask about the boat\n");
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.conv_goals_context = goals;
    cfg.conv_goals_context_len = n;
    size_t len = 0;
    hu_immersive_context_stats_t st;
    char *out = compose(&cfg, HU_IMMERSIVE_CONTEXT_BUDGET_BYTES, &len, &st);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(st.truncated);
    HU_ASSERT_NULL(strstr(out, "CONT_MARKER"));
    HU_ASSERT_NULL(strstr(out, "xxxx"));
    HU_ASSERT_NOT_NULL(strstr(out, "- NEXT_GOAL_MARKER ask about the boat\n"));
    free_block(out, len);
}

/* A budget too small for any item composes nothing — never a partial line. */
static void immersive_context_tiny_budget_composes_nothing(void) {
    hu_prompt_config_t cfg = full_cfg();
    size_t len = 0;
    hu_immersive_context_stats_t st;
    char *out = compose(&cfg, 40, &len, &st);
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

static char *build_immersive(const char *gate, bool local, size_t *len) {
    hu_allocator_t a = hu_system_allocator();
    hu_prompt_config_t cfg = full_cfg();
    cfg.persona_immersive = true;
    cfg.persona_prompt = "PERSONA_HEAD you text like yourself.";
    cfg.persona_prompt_len = strlen(cfg.persona_prompt);
    cfg.memory_context = "MEMORY_MARKER she moved to Tampa";
    cfg.memory_context_len = strlen(cfg.memory_context);
    cfg.suppress_prompt_budget_diagnostic = true;
    cfg.private_context_local = local;
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
    char *unset = build_immersive(NULL, true, &l_unset);
    char *off = build_immersive("off", true, &l_off);
    char *shadow = build_immersive("shadow", true, &l_shadow);
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
    char *off = build_immersive(NULL, true, &l_off);
    char *live = build_immersive("live", true, &l_live);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_NOT_NULL(live);
    HU_ASSERT_NULL(strstr(off, "call my sister tomorrow"));
    const char *block = strstr(live, HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT);
    HU_ASSERT_NOT_NULL(block);
    HU_ASSERT_NOT_NULL(strstr(block, "- call my sister tomorrow (their words)\n"));
    /* Ahead of the trimmable middle: the block precedes memory context. */
    HU_ASSERT_TRUE(block < strstr(live, "MEMORY_MARKER"));
    HU_ASSERT_TRUE(l_live > l_off);
    size_t stripped = hu_private_context_strip(live, l_live);
    HU_ASSERT_EQ(stripped, l_off);
    HU_ASSERT_STR_EQ(live, off);
    free_prompt(off, l_off);
    free_prompt(live, l_live);
}

/* Not local (a cloud provider or model): LIVE composes nothing at all. */
static void immersive_context_live_not_local_is_off(void) {
    size_t l_off = 0, l_live = 0;
    char *off = build_immersive(NULL, true, &l_off);
    char *live = build_immersive("live", false, &l_live);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_NOT_NULL(live);
    HU_ASSERT_NULL(strstr(live, HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT));
    HU_ASSERT_STR_EQ(live, off);
    free_prompt(off, l_off);
    free_prompt(live, l_live);
}

void run_immersive_context_tests(void) {
    HU_TEST_SUITE("ImmersiveContext");
    HU_RUN_TEST(immersive_context_empty_input_composes_nothing);
    HU_RUN_TEST(immersive_context_chrome_and_directives_compose_nothing);
    HU_RUN_TEST(immersive_context_keeps_facts_in_priority_order);
    HU_RUN_TEST(immersive_context_drops_what_core_memory_already_says);
    HU_RUN_TEST(immersive_context_budget_drops_whole_items);
    HU_RUN_TEST(immersive_context_oversize_item_swallows_its_continuations);
    HU_RUN_TEST(immersive_context_tiny_budget_composes_nothing);
    HU_RUN_TEST(immersive_context_gate_off_and_shadow_keep_prompt_identical);
    HU_RUN_TEST(immersive_context_gate_live_adds_exactly_the_block);
    HU_RUN_TEST(immersive_context_live_not_local_is_off);
}
