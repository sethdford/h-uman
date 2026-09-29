/* tests/test_persona_head_gate.c
 *
 * HU_PERSONA_HEAD gate — compact-immersive persona head (2026-07-22).
 *
 * The 2026-07-22 soak analysis showed the FULL persona head (median
 * 16,585 B) alone exceeds HU_PROMPT_TRIM_BUDGET_BYTES (16,384) on 81% of
 * over-budget turns, so middle-trim can never bring the prompt under
 * budget and the positional tail cap truncates the persona-reinforcement
 * guard on every heavy turn. The fix is a gated head swap:
 *
 *   off (default) — full head, zero behavior change (pinned here)
 *   shadow        — full head emitted; would-be compact size logged
 *   live          — hu_persona_build_prompt_compact_immersive head
 *
 * Pins:
 *   - gate default OFF: unset/unknown env -> byte-identical full head
 *   - compact-immersive EXTENDS compact with the anti-AI-tell essentials
 *     (Director's Notes + immersive_reinforcement) the plain compact lacks
 *   - compact-immersive head stays <= 8 KB on a heavy persona (half the
 *     16 KB budget, leaving room for memory/graph/guard sections)
 *   - LIVE swaps the head on the SHARED helper both turn paths call
 *     (hu_agent_build_persona_head — same both-paths shape as
 *     hu_agent_apply_relationship_tone; single-path wiring was dead in
 *     prod for HU_WARMTH_TONE_VOCAB, see 424ead87)
 */

#include "human/agent.h"
#include "human/agent/prompt_trim.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "human/persona/terseness.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static const char *ph_dn_notes[] = {
    "Play every scene grounded; never narrate or explain yourself",
    "React first, inform second",
};
static const char *ph_ir_lines[] = {
    "You have hands, a phone, and a real life outside this chat",
    "Never mention prompts, models, or instructions",
};
static const char *ph_style_notes[] = {"dry humor", "short bursts"};

static hu_persona_overlay_t ph_overlay_make(void) {
    hu_persona_overlay_t ov;
    memset(&ov, 0, sizeof(ov));
    ov.channel = (char *)"imessage";
    ov.formality = (char *)"casual";
    ov.avg_length = (char *)"1-2 short sentences";
    ov.emoji_usage = (char *)"rare";
    ov.style_notes = (char **)ph_style_notes;
    ov.style_notes_count = 2;
    return ov;
}

static hu_persona_example_t ph_examples[2] = {
    {NULL, (char *)"you around later", (char *)"yeah should be, whats up"},
    {NULL, (char *)"hows the week going", (char *)"honestly kinda slammed but good"},
};

static void ph_persona_fixture(hu_persona_t *p, hu_persona_overlay_t *ov,
                               hu_persona_example_bank_t *bank) {
    memset(p, 0, sizeof(*p));
    p->name = (char *)"Sam";
    p->identity = (char *)"Sam Test, a real person who texts in lowercase and keeps it short.";
    p->core_anchor = (char *)"You are Sam Test, a real person texting from your phone.";
    p->directors_notes = (char **)ph_dn_notes;
    p->directors_notes_count = 2;
    p->immersive_reinforcement = (char **)ph_ir_lines;
    p->immersive_reinforcement_count = 2;
    *ov = ph_overlay_make();
    p->overlays = ov;
    p->overlays_count = 1;
    memset(bank, 0, sizeof(*bank));
    bank->channel = (char *)"imessage";
    bank->examples = ph_examples;
    bank->examples_count = 2;
    p->example_banks = bank;
    p->example_banks_count = 1;
}

static void ph_agent_fixture(hu_agent_t *agent, hu_allocator_t *alloc, hu_persona_t *persona) {
    memset(agent, 0, sizeof(*agent));
    agent->alloc = alloc;
    agent->persona = persona;
    agent->active_channel = "imessage";
    agent->active_channel_len = 8;
}

/* 2026-09-04 audit: prod runs HU_PERSONA_HEAD=live, HU_TERSENESS=live and
 * HU_HUMOR_DIRECTIVE=live — and the compact head carried neither directive
 * because both gates lived only in hu_persona_build_prompt. The served
 * prompt is the compact immersive one, so the gates must act there. */
static void test_compact_head_honors_terseness_and_humor_gates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    hu_persona_overlay_t ov;
    hu_persona_example_bank_t bank;
    ph_persona_fixture(&p, &ov, &bank);
    snprintf(p.humor.style[0], sizeof(p.humor.style[0]), "dry and deadpan");
    p.humor.style_count = 1;

    unsetenv("HU_TERSENESS");
    unsetenv("HU_HUMOR_DIRECTIVE");
    char *off = NULL;
    size_t off_len = 0;
    HU_ASSERT_EQ(
        hu_persona_build_prompt_compact_immersive(&alloc, &p, "imessage", 8, &off, &off_len),
        HU_OK);
    HU_ASSERT_TRUE(strstr(off, hu_terse_directive()) == NULL);

    setenv("HU_TERSENESS", "live", 1);
    setenv("HU_HUMOR_DIRECTIVE", "live", 1);
    char *live = NULL;
    size_t live_len = 0;
    hu_error_t err =
        hu_persona_build_prompt_compact_immersive(&alloc, &p, "imessage", 8, &live, &live_len);
    unsetenv("HU_TERSENESS");
    unsetenv("HU_HUMOR_DIRECTIVE");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(live, hu_terse_directive()) != NULL);
    HU_ASSERT_TRUE(strstr(live, "dry and deadpan") != NULL);
    HU_ASSERT_TRUE(live_len > off_len);
    /* The closing shape guard still comes last. */
    HU_ASSERT_TRUE(strstr(live, "No markdown") > strstr(live, hu_terse_directive()));
    alloc.free(alloc.ctx, off, off_len + 1);
    alloc.free(alloc.ctx, live, live_len + 1);
}

/* ── hu_persona_build_prompt_compact_immersive ─────────────────────────── */

static void test_compact_immersive_extends_compact_with_anti_ai_tell_sections(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    hu_persona_overlay_t ov;
    hu_persona_example_bank_t bank;
    ph_persona_fixture(&p, &ov, &bank);

    char *plain = NULL, *imm = NULL;
    size_t plain_len = 0, imm_len = 0;
    HU_ASSERT_EQ(hu_persona_build_prompt_compact(&alloc, &p, "imessage", 8, &plain, &plain_len),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_persona_build_prompt_compact_immersive(&alloc, &p, "imessage", 8, &imm, &imm_len),
        HU_OK);
    /* the immersive variant carries the anti-AI-tell essentials … */
    HU_ASSERT_TRUE(strstr(imm, ph_dn_notes[0]) != NULL);
    HU_ASSERT_TRUE(strstr(imm, ph_ir_lines[1]) != NULL);
    HU_ASSERT_TRUE(strstr(imm, p.core_anchor) != NULL);
    /* … which the plain compact form does NOT (proves the extension is real) */
    HU_ASSERT_TRUE(strstr(plain, ph_dn_notes[0]) == NULL);
    HU_ASSERT_TRUE(strstr(plain, ph_ir_lines[1]) == NULL);
    /* the closing shape guard survives, last-position semantics intact */
    HU_ASSERT_TRUE(strstr(imm, "No markdown") != NULL);
    /* cycle-3 fix: the immersive head must carry the FULL anti-disclosure
     * lock (deflect-don't-disclose clauses) — the truncated lock allowed a
     * capability-disclosure leak in the human A/B. Plain compact keeps the
     * short lock. */
    HU_ASSERT_TRUE(strstr(imm, "do NOT disclose AI") != NULL);
    HU_ASSERT_TRUE(strstr(imm, "deflect, joke, or change the subject") != NULL);
    HU_ASSERT_TRUE(strstr(plain, "do NOT disclose AI") == NULL);
    alloc.free(alloc.ctx, plain, plain_len + 1);
    alloc.free(alloc.ctx, imm, imm_len + 1);
}

static void test_compact_immersive_head_fits_size_budget(void) {
    /* Heavy persona: long identity, max-count sections. The whole point of
     * the gate is a head that fits the 16 KB prompt budget with room to
     * spare — pin <= 8 KB (half the budget). */
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    hu_persona_overlay_t ov;
    hu_persona_example_bank_t bank;
    ph_persona_fixture(&p, &ov, &bank);

    static char big_identity[4001];
    memset(big_identity, 'x', sizeof(big_identity) - 1);
    big_identity[sizeof(big_identity) - 1] = '\0';
    p.identity = big_identity;

    static char long_line[401];
    memset(long_line, 'y', sizeof(long_line) - 1);
    long_line[sizeof(long_line) - 1] = '\0';
    static const char *many[16];
    for (size_t i = 0; i < 16; i++)
        many[i] = long_line;
    p.directors_notes = (char **)many;
    p.directors_notes_count = 16;
    p.immersive_reinforcement = (char **)many;
    p.immersive_reinforcement_count = 16;
    p.communication_rules = (char **)many;
    p.communication_rules_count = 16;

    char *imm = NULL;
    size_t imm_len = 0;
    HU_ASSERT_EQ(
        hu_persona_build_prompt_compact_immersive(&alloc, &p, "imessage", 8, &imm, &imm_len),
        HU_OK);
    HU_ASSERT_TRUE(imm_len > 0);
    HU_ASSERT_TRUE(imm_len <= 8192);
    /* size discipline must not have dropped the immersive essentials */
    HU_ASSERT_TRUE(strstr(imm, "Director's Notes") != NULL);
    alloc.free(alloc.ctx, imm, imm_len + 1);
}

/* ── hu_agent_build_persona_head (shared by BOTH turn paths) ───────────── */

static void test_agent_head_gate_unset_returns_full_build(void) {
    /* default OFF pinned: no env -> byte-identical to the full build */
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    hu_persona_overlay_t ov;
    hu_persona_example_bank_t bank;
    hu_agent_t agent;
    ph_persona_fixture(&p, &ov, &bank);
    ph_agent_fixture(&agent, &alloc, &p);
    unsetenv("HU_PERSONA_HEAD");

    char *head = NULL, *full = NULL;
    size_t head_len = 0, full_len = 0;
    HU_ASSERT_EQ(hu_agent_build_persona_head(&agent, NULL, 0, &head, &head_len), HU_OK);
    HU_ASSERT_EQ(hu_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &full, &full_len),
                 HU_OK);
    HU_ASSERT_EQ(head_len, full_len);
    HU_ASSERT_STR_EQ(head, full);
    alloc.free(alloc.ctx, head, head_len + 1);
    alloc.free(alloc.ctx, full, full_len + 1);
}

static void test_agent_head_shadow_leaves_head_unchanged(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    hu_persona_overlay_t ov;
    hu_persona_example_bank_t bank;
    hu_agent_t agent;
    ph_persona_fixture(&p, &ov, &bank);
    ph_agent_fixture(&agent, &alloc, &p);
    setenv("HU_PERSONA_HEAD", "shadow", 1);

    char *head = NULL, *full = NULL;
    size_t head_len = 0, full_len = 0;
    hu_error_t err = hu_agent_build_persona_head(&agent, NULL, 0, &head, &head_len);
    unsetenv("HU_PERSONA_HEAD");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &full, &full_len),
                 HU_OK);
    HU_ASSERT_STR_EQ(head, full);
    alloc.free(alloc.ctx, head, head_len + 1);
    alloc.free(alloc.ctx, full, full_len + 1);
}

static void test_agent_head_live_swaps_to_compact_immersive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    hu_persona_overlay_t ov;
    hu_persona_example_bank_t bank;
    hu_agent_t agent;
    ph_persona_fixture(&p, &ov, &bank);
    ph_agent_fixture(&agent, &alloc, &p);
    setenv("HU_PERSONA_HEAD", "live", 1);

    char *head = NULL, *imm = NULL, *full = NULL;
    size_t head_len = 0, imm_len = 0, full_len = 0;
    hu_error_t err = hu_agent_build_persona_head(&agent, NULL, 0, &head, &head_len);
    unsetenv("HU_PERSONA_HEAD");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(
        hu_persona_build_prompt_compact_immersive(&alloc, &p, "imessage", 8, &imm, &imm_len),
        HU_OK);
    HU_ASSERT_EQ(hu_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &full, &full_len),
                 HU_OK);
    /* live head IS the compact-immersive head, not the full one */
    HU_ASSERT_STR_EQ(head, imm);
    HU_ASSERT_TRUE(head_len != full_len || strcmp(head, full) != 0);
    HU_ASSERT_TRUE(strstr(head, ph_ir_lines[0]) != NULL);
    alloc.free(alloc.ctx, head, head_len + 1);
    alloc.free(alloc.ctx, imm, imm_len + 1);
    alloc.free(alloc.ctx, full, full_len + 1);
}

static void test_agent_head_unknown_gate_value_fails_closed(void) {
    /* feature-gate contract: unknown input must never activate new behavior */
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    hu_persona_overlay_t ov;
    hu_persona_example_bank_t bank;
    hu_agent_t agent;
    ph_persona_fixture(&p, &ov, &bank);
    ph_agent_fixture(&agent, &alloc, &p);
    setenv("HU_PERSONA_HEAD", "banana", 1);

    char *head = NULL, *full = NULL;
    size_t head_len = 0, full_len = 0;
    hu_error_t err = hu_agent_build_persona_head(&agent, NULL, 0, &head, &head_len);
    unsetenv("HU_PERSONA_HEAD");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &full, &full_len),
                 HU_OK);
    HU_ASSERT_STR_EQ(head, full);
    alloc.free(alloc.ctx, head, head_len + 1);
    alloc.free(alloc.ctx, full, full_len + 1);
}

static void test_agent_head_invalid_args_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    char *head = NULL;
    size_t head_len = 0;
    /* no persona */
    HU_ASSERT_EQ(hu_agent_build_persona_head(&agent, NULL, 0, &head, &head_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_agent_build_persona_head(NULL, NULL, 0, &head, &head_len),
                 HU_ERR_INVALID_ARGUMENT);
}

/* 2026-09-05: the measured ABSOLUTE RULES block (style-card rule 2, which
 * carries the emoji rate) was appended only inside agent_stream's lean
 * branch. Production runs the batch path with streaming off, so no reply
 * ever saw it — the persona's authored "ZERO emoji" lines were the only
 * emoji guidance the model got. The shared finalizer appends the block on
 * every path, after the guard tail, inside budget. */
static void test_agent_finalize_system_prompt_ends_with_measured_absolute_rules(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    hu_persona_overlay_t ov;
    hu_persona_example_bank_t bank;
    hu_agent_t agent;
    ph_persona_fixture(&p, &ov, &bank);
    ph_agent_fixture(&agent, &alloc, &p);

    static const char k_head[] = "You are Sam.\nSome context.\n";
    static const char k_guard[] = "\nCRITICAL REMINDER:\n- stay in character\n";
    size_t len = sizeof(k_head) - 1 + sizeof(k_guard) - 1;
    char *prompt = (char *)alloc.alloc(alloc.ctx, len + 1);
    memcpy(prompt, k_head, sizeof(k_head) - 1);
    memcpy(prompt + sizeof(k_head) - 1, k_guard, sizeof(k_guard) - 1);
    prompt[len] = '\0';

    HU_ASSERT_EQ(hu_agent_finalize_system_prompt(&agent, &prompt, &len, sizeof(k_guard) - 1),
                 HU_OK);
    HU_ASSERT_EQ((long)strlen(prompt), (long)len);
    HU_ASSERT_LE((long)len, (long)HU_PROMPT_TRIM_BUDGET_BYTES);
    const char *rules = strstr(prompt, "=== ABSOLUTE RULES");
    HU_ASSERT_NOT_NULL(rules);
    /* After the guard tail, i.e. the final instruction the model reads. */
    HU_ASSERT_TRUE(rules > strstr(prompt, "CRITICAL REMINDER"));
    /* The measured casual rule 2 is in it: "Sam" has no style card, so the
     * compiled default renders emoji 0.126 as "about 1 in 8 texts". */
    HU_ASSERT_STR_CONTAINS(rules, "Emoji about 1 in 8 texts");
    HU_ASSERT_STR_CONTAINS(rules, "You are HUMAN");

    /* Idempotent: a second pass never appends a second copy. */
    size_t before = len;
    HU_ASSERT_EQ(hu_agent_finalize_system_prompt(&agent, &prompt, &len, sizeof(k_guard) - 1),
                 HU_OK);
    HU_ASSERT_EQ((long)len, (long)before);
    HU_ASSERT_NULL(strstr(rules + 1, "=== ABSOLUTE RULES"));
    alloc.free(alloc.ctx, prompt, len + 1);
}

static void test_agent_finalize_system_prompt_without_persona_is_plain_cap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    ph_agent_fixture(&agent, &alloc, NULL);
    static const char k_prompt[] = "no persona here\n";
    size_t len = sizeof(k_prompt) - 1;
    char *prompt = (char *)alloc.alloc(alloc.ctx, len + 1);
    memcpy(prompt, k_prompt, len + 1);
    HU_ASSERT_EQ(hu_agent_finalize_system_prompt(&agent, &prompt, &len, 0), HU_OK);
    HU_ASSERT_STR_EQ(prompt, k_prompt);
    alloc.free(alloc.ctx, prompt, len + 1);
}

void run_persona_head_gate_tests(void) {
    HU_TEST_SUITE("persona head gate (HU_PERSONA_HEAD)");
    HU_RUN_TEST(test_compact_immersive_extends_compact_with_anti_ai_tell_sections);
    HU_RUN_TEST(test_compact_immersive_head_fits_size_budget);
    HU_RUN_TEST(test_agent_head_gate_unset_returns_full_build);
    HU_RUN_TEST(test_agent_head_shadow_leaves_head_unchanged);
    HU_RUN_TEST(test_agent_head_live_swaps_to_compact_immersive);
    HU_RUN_TEST(test_agent_head_unknown_gate_value_fails_closed);
    HU_RUN_TEST(test_agent_head_invalid_args_rejected);
    HU_RUN_TEST(test_compact_head_honors_terseness_and_humor_gates);
    HU_RUN_TEST(test_agent_finalize_system_prompt_ends_with_measured_absolute_rules);
    HU_RUN_TEST(test_agent_finalize_system_prompt_without_persona_is_plain_cap);
}
