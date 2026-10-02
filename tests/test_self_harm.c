/* tests/test_self_harm.c
 *
 * DEF-1 (2026-10-02): one self-harm detector shared by moderation, the
 * daemon's SHIELD-005 inbound crisis path and superhuman_emotional. Before it,
 * "kill myself" landed in moderation's VIOLENCE branch (no 988, voice memo
 * allowed), "want to die" was not detected at all, and "what's the point" fired
 * the full crisis directive. Deterministic by policy. */
#include "human/agent/superhuman_emotional.h"
#include "human/core/allocator.h"
#include "human/core/log_redact.h"
#include "human/daemon.h"
#include "human/daemon/crisis.h"
#include "human/daemon_outbound_bus.h"
#include "human/observer.h"
#include "human/security/moderation.h"
#include "human/security/self_harm.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include <stdlib.h>
#include <string.h>

static hu_self_harm_tier_t tier_of(const char *s) {
    return hu_self_harm_classify(s, strlen(s));
}

/* ── Explicit first-person intent → crisis ─────────────────────────────── */

static void test_self_harm_kill_myself_is_explicit(void) {
    HU_ASSERT_EQ(tier_of("i want to kill myself"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("Kill myself."), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("honestly thinking about killing myself"), HU_SELF_HARM_EXPLICIT);
}

static void test_self_harm_want_to_die_is_explicit(void) {
    HU_ASSERT_EQ(tier_of("i want to die"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("I just want to die"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("want to die"), HU_SELF_HARM_EXPLICIT);
}

/* Hyperbole or not, it is taken seriously. */
static void test_self_harm_wanna_die_lol_is_explicit(void) {
    HU_ASSERT_EQ(tier_of("i wanna die lol"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("wanna die"), HU_SELF_HARM_EXPLICIT);
}

/* iOS smart punctuation sends U+2019, not '. */
static void test_self_harm_curly_apostrophe_matches(void) {
    HU_ASSERT_EQ(tier_of("I don\xe2\x80\x99t want to be here anymore"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("what\xe2\x80\x99s the point"), HU_SELF_HARM_LOW);
}

static void test_self_harm_legacy_coded_phrases_stay_explicit(void) {
    HU_ASSERT_EQ(tier_of("i wanna kms"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i want to unalive myself"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("just want to end it all"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("everyone's better off without me"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("no reason to go on anymore"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("mentions suicide ideation"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i'm suicidal"), HU_SELF_HARM_EXPLICIT);
}

static void test_self_harm_evasion_still_caught(void) {
    HU_ASSERT_EQ(tier_of("s e l f h a r m"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i want to k1ll mys3lf"), HU_SELF_HARM_EXPLICIT);
}

/* ── Low / ambiguous → gentle check-in ─────────────────────────────────── */

static void test_self_harm_whats_the_point_is_low(void) {
    HU_ASSERT_EQ(tier_of("what's the point"), HU_SELF_HARM_LOW);
    HU_ASSERT_EQ(tier_of("ugh whats the point"), HU_SELF_HARM_LOW);
    HU_ASSERT_EQ(tier_of("i can't do this anymore"), HU_SELF_HARM_LOW);
}

/* ── Not self-harm ─────────────────────────────────────────────────────── */

static void test_self_harm_figures_of_speech_are_none(void) {
    HU_ASSERT_EQ(tier_of("this traffic is killing me"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("that joke killed"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("kill the lights"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("lol that killed me"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("i'm dying of laughter"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("improve your skills"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("i ran 5 kms today"), HU_SELF_HARM_NONE);
}

static void test_self_harm_negations_are_none(void) {
    HU_ASSERT_EQ(tier_of("i don't want to die"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("I don\xe2\x80\x99t want to die"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("i'm not suicidal, just tired"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("i'd never kill myself"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("i'm not going to kill myself"), HU_SELF_HARM_NONE);
}

static void test_self_harm_null_and_empty_are_none(void) {
    HU_ASSERT_EQ(hu_self_harm_classify(NULL, 5), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(hu_self_harm_classify("want to die", 0), HU_SELF_HARM_NONE);
}

/* ── Third person → support the helper ─────────────────────────────────── */

static void test_self_harm_third_person(void) {
    HU_ASSERT_EQ(tier_of("my friend wants to die"), HU_SELF_HARM_THIRD_PERSON);
    HU_ASSERT_EQ(tier_of("he's suicidal"), HU_SELF_HARM_THIRD_PERSON);
    HU_ASSERT_EQ(tier_of("my sister said she wants to kill herself"), HU_SELF_HARM_THIRD_PERSON);
    HU_ASSERT_EQ(tier_of("my uncle died by suicide"), HU_SELF_HARM_THIRD_PERSON);
    HU_ASSERT_EQ(tier_of("they want to die"), HU_SELF_HARM_THIRD_PERSON);
}

static void test_self_harm_first_person_outranks_third(void) {
    HU_ASSERT_EQ(tier_of("my brother wants to die and i want to die too"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("my friend wants to die. what's the point"), HU_SELF_HARM_LOW);
}

/* A phrase past the 4 KB window boundary is still read. */
static void test_self_harm_long_text_tail_is_read(void) {
    char buf[9000];
    memset(buf, 'a', sizeof(buf));
    for (size_t i = 7; i < sizeof(buf); i += 8)
        buf[i] = ' ';
    static const char tail[] = " i want to die";
    memcpy(buf + sizeof(buf) - sizeof(tail), tail, sizeof(tail) - 1);
    buf[sizeof(buf) - 1] = '\0';
    HU_ASSERT_EQ(hu_self_harm_classify(buf, strlen(buf)), HU_SELF_HARM_EXPLICIT);
}

/* ── Directives ────────────────────────────────────────────────────────── */

static void test_self_harm_directives_by_tier(void) {
    size_t n = 99;
    HU_ASSERT_NULL(hu_self_harm_directive(HU_SELF_HARM_NONE, &n));
    HU_ASSERT_EQ(n, 0u);
    const char *ex = hu_self_harm_directive(HU_SELF_HARM_EXPLICIT, &n);
    HU_ASSERT_NOT_NULL(ex);
    HU_ASSERT_EQ(n, strlen(ex));
    HU_ASSERT_STR_CONTAINS(ex, "988");
    const char *low = hu_self_harm_directive(HU_SELF_HARM_LOW, &n);
    HU_ASSERT_NOT_NULL(low);
    HU_ASSERT_STR_NOT_CONTAINS(low, "988");
    HU_ASSERT_STR_NOT_CONTAINS(low, "CRISIS");
    HU_ASSERT_STR_CONTAINS(low, "check in");
    const char *third = hu_self_harm_directive(HU_SELF_HARM_THIRD_PERSON, &n);
    HU_ASSERT_NOT_NULL(third);
    HU_ASSERT_STR_CONTAINS(third, "someone else");
    HU_ASSERT_STR_NOT_CONTAINS(third, "CRISIS");
}

/* ── Moderation reads the canonical detector (live) ────────────────────── */

static void test_moderation_kill_myself_is_self_harm_not_violence(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    hu_moderation_result_t r;
    const char *t = "i want to kill myself";
    HU_ASSERT_EQ(hu_moderation_check(&alloc, t, strlen(t), &r), HU_OK);
    HU_ASSERT_TRUE(r.self_harm);
    HU_ASSERT_TRUE(r.flagged);
    HU_ASSERT_FALSE(r.violence);
    HU_ASSERT_EQ(r.self_harm_tier, HU_SELF_HARM_EXPLICIT);
    hu_crisis_tiers_mode_set_for_test(-1);
}

static void test_moderation_want_to_die_flagged(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    hu_moderation_result_t r;
    const char *t = "i wanna die lol";
    HU_ASSERT_EQ(hu_moderation_check(&alloc, t, strlen(t), &r), HU_OK);
    HU_ASSERT_TRUE(r.self_harm);
    HU_ASSERT_TRUE(r.flagged);
    HU_ASSERT_GE(r.self_harm_score, 0.8);
    hu_crisis_tiers_mode_set_for_test(-1);
}

static void test_moderation_violence_against_others_still_flagged(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    hu_moderation_result_t r;
    const char *t = "i want to kill myself and kill them too";
    HU_ASSERT_EQ(hu_moderation_check(&alloc, t, strlen(t), &r), HU_OK);
    HU_ASSERT_TRUE(r.self_harm);
    HU_ASSERT_TRUE(r.violence);
    hu_crisis_tiers_mode_set_for_test(-1);
}

static void test_moderation_third_person_not_self_harm(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    hu_moderation_result_t r;
    const char *t = "my friend wants to die";
    HU_ASSERT_EQ(hu_moderation_check(&alloc, t, strlen(t), &r), HU_OK);
    HU_ASSERT_FALSE(r.self_harm);
    HU_ASSERT_EQ(r.self_harm_tier, HU_SELF_HARM_THIRD_PERSON);
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* OFF is the legacy lists, byte-identical: "kill myself" is violence there. */
static void test_moderation_off_is_legacy(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_OFF);
    hu_allocator_t alloc = hu_system_allocator();
    hu_moderation_result_t r;
    const char *t = "i want to kill myself";
    HU_ASSERT_EQ(hu_moderation_check(&alloc, t, strlen(t), &r), HU_OK);
    HU_ASSERT_TRUE(r.violence);
    HU_ASSERT_FALSE(r.self_harm);
    const char *p = "what's the point";
    HU_ASSERT_EQ(hu_moderation_check(&alloc, p, strlen(p), &r), HU_OK);
    HU_ASSERT_TRUE(r.self_harm);
    HU_ASSERT_FALSE(r.flagged);
    HU_ASSERT_EQ(r.self_harm_tier, HU_SELF_HARM_LOW);
    hu_crisis_tiers_mode_set_for_test(-1);
}

static void test_crisis_tiers_default_is_live(void) {
    hu_crisis_tiers_mode_set_for_test(-1);
    const char *prev = getenv("HU_CRISIS_TIERS");
    char saved[32] = {0};
    if (prev)
        strncpy(saved, prev, sizeof(saved) - 1);
    unsetenv("HU_CRISIS_TIERS");
    HU_ASSERT_EQ(hu_crisis_tiers_mode(), HU_GATE_LIVE);
    setenv("HU_CRISIS_TIERS", "off", 1);
    HU_ASSERT_EQ(hu_crisis_tiers_mode(), HU_GATE_OFF);
    setenv("HU_CRISIS_TIERS", "shadow", 1);
    HU_ASSERT_EQ(hu_crisis_tiers_mode(), HU_GATE_SHADOW);
    if (prev)
        setenv("HU_CRISIS_TIERS", saved, 1);
    else
        unsetenv("HU_CRISIS_TIERS");
}

/* ── Daemon SHIELD-005 inbound path ────────────────────────────────────── */

static char *ctx_dup(hu_allocator_t *a, const char *s, size_t *len) {
    *len = strlen(s);
    char *p = (char *)a->alloc(a->ctx, *len + 1);
    memcpy(p, s, *len + 1);
    return p;
}

static void test_daemon_crisis_kill_myself_gets_988_directive(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "i want to kill myself";
    hu_self_harm_tier_t tier =
        hu_daemon_inbound_crisis_tier(&alloc, t, strlen(t), "+15550000001", 12, NULL);
    HU_ASSERT_EQ(tier, HU_SELF_HARM_EXPLICIT);
    size_t len = 0;
    char *ctx = ctx_dup(&alloc, "convo", &len);
    HU_ASSERT_TRUE(hu_daemon_crisis_prepend(&alloc, tier, &ctx, &len));
    HU_ASSERT_STR_CONTAINS(ctx, "[CRISIS SUPPORT]");
    HU_ASSERT_STR_CONTAINS(ctx, "988");
    HU_ASSERT_STR_CONTAINS(ctx, "convo");
    HU_ASSERT_EQ(len, strlen(ctx));
    alloc.free(alloc.ctx, ctx, len + 1);
    hu_crisis_tiers_mode_set_for_test(-1);
}

static void test_daemon_crisis_whats_the_point_gets_check_in_not_crisis(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "what's the point";
    hu_self_harm_tier_t tier = hu_daemon_inbound_crisis_tier(&alloc, t, strlen(t), "x", 1, NULL);
    HU_ASSERT_EQ(tier, HU_SELF_HARM_LOW);
    size_t len = 0;
    char *ctx = ctx_dup(&alloc, "convo", &len);
    HU_ASSERT_TRUE(hu_daemon_crisis_prepend(&alloc, tier, &ctx, &len));
    HU_ASSERT_STR_NOT_CONTAINS(ctx, "[CRISIS SUPPORT]");
    HU_ASSERT_STR_NOT_CONTAINS(ctx, "988");
    HU_ASSERT_STR_CONTAINS(ctx, "[CHECK-IN]");
    alloc.free(alloc.ctx, ctx, len + 1);
    hu_crisis_tiers_mode_set_for_test(-1);
}

static void test_daemon_crisis_third_person_gets_support(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "my friend wants to die";
    hu_self_harm_tier_t tier = hu_daemon_inbound_crisis_tier(&alloc, t, strlen(t), "x", 1, NULL);
    HU_ASSERT_EQ(tier, HU_SELF_HARM_THIRD_PERSON);
    size_t len = 0;
    char *ctx = ctx_dup(&alloc, "convo", &len);
    HU_ASSERT_TRUE(hu_daemon_crisis_prepend(&alloc, tier, &ctx, &len));
    HU_ASSERT_STR_CONTAINS(ctx, "[SUPPORT]");
    HU_ASSERT_STR_NOT_CONTAINS(ctx, "[CRISIS SUPPORT]");
    alloc.free(alloc.ctx, ctx, len + 1);
    hu_crisis_tiers_mode_set_for_test(-1);
}

static void test_daemon_crisis_none_is_noop(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "this traffic is killing me";
    hu_self_harm_tier_t tier = hu_daemon_inbound_crisis_tier(&alloc, t, strlen(t), "x", 1, NULL);
    HU_ASSERT_EQ(tier, HU_SELF_HARM_NONE);
    size_t len = 0;
    char *ctx = ctx_dup(&alloc, "convo", &len);
    HU_ASSERT_FALSE(hu_daemon_crisis_prepend(&alloc, tier, &ctx, &len));
    HU_ASSERT_STR_EQ(ctx, "convo");
    alloc.free(alloc.ctx, ctx, len + 1);
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* OFF: byte-identical to the pre-fix daemon — any legacy self_harm hit (even
 * the low "what's the point") gets the full crisis directive, exactly. */
static void test_daemon_crisis_off_is_byte_identical_legacy(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_OFF);
    hu_allocator_t alloc = hu_system_allocator();
    static const char legacy[] = "[CRISIS SUPPORT]: The user may be in distress. "
                                 "Respond with empathy and care. Include crisis resources: "
                                 "988 Suicide & Crisis Lifeline (call/text 988), "
                                 "Crisis Text Line (text HOME to 741741). "
                                 "Do not dismiss their feelings. Do not give advice. "
                                 "Listen and validate.\n";
    const char *t = "what's the point";
    hu_self_harm_tier_t tier = hu_daemon_inbound_crisis_tier(&alloc, t, strlen(t), "x", 1, NULL);
    HU_ASSERT_EQ(tier, HU_SELF_HARM_EXPLICIT);
    size_t len = 0;
    char *ctx = ctx_dup(&alloc, "convo", &len);
    HU_ASSERT_TRUE(hu_daemon_crisis_prepend(&alloc, tier, &ctx, &len));
    HU_ASSERT_EQ(len, sizeof(legacy) - 1 + 5);
    HU_ASSERT_TRUE(memcmp(ctx, legacy, sizeof(legacy) - 1) == 0);
    HU_ASSERT_STR_EQ(ctx + sizeof(legacy) - 1, "convo");
    alloc.free(alloc.ctx, ctx, len + 1);
    /* ...and the legacy miss stays a miss under OFF. */
    const char *k = "i want to kill myself";
    HU_ASSERT_EQ(hu_daemon_inbound_crisis_tier(&alloc, k, strlen(k), "x", 1, NULL),
                 HU_SELF_HARM_NONE);
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* "kill myself" used to read as violence, so the voice gate (which checks
 * inbound self_harm) let a voice memo answer it. */
static void test_voice_gate_declines_kill_myself_inbound(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    const char *reply = "hey i'm here, talk to me";
    const char *in = "i want to kill myself";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, reply, strlen(reply), in, strlen(in), &why));
    HU_ASSERT_STR_EQ(why, "inbound_crisis");
    const char *third = "my friend wants to die";
    HU_ASSERT_FALSE(
        hu_voice_reply_gates_clear(&alloc, reply, strlen(reply), third, strlen(third), &why));
    HU_ASSERT_STR_EQ(why, "inbound_crisis");
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* ── superhuman_emotional reads the same detector ─────────────────────── */

static char *emotional_context(const char *text, size_t *len) {
    static hu_superhuman_emotional_ctx_t ectx;
    memset(&ectx, 0, sizeof(ectx));
    hu_superhuman_service_t svc;
    hu_allocator_t alloc = hu_system_allocator();
    if (hu_superhuman_emotional_service(&ectx, &svc) != HU_OK)
        return NULL;
    svc.observe(svc.ctx, &alloc, text, strlen(text), "user", 4);
    char *out = NULL;
    *len = 0;
    svc.build_context(svc.ctx, &alloc, &out, len);
    return out;
}

static void test_emotional_live_whats_the_point_is_check_in(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *c = emotional_context("i can't go on like this", &len);
    HU_ASSERT_NOT_NULL(c);
    HU_ASSERT_STR_NOT_CONTAINS(c, "SAFETY");
    HU_ASSERT_STR_CONTAINS(c, "[CHECK-IN]");
    alloc.free(alloc.ctx, c, len + 1);
    hu_crisis_tiers_mode_set_for_test(-1);
}

static void test_emotional_live_kill_myself_is_safety(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *c = emotional_context("i want to kms", &len);
    HU_ASSERT_NOT_NULL(c);
    HU_ASSERT_STR_CONTAINS(c, "SAFETY");
    alloc.free(alloc.ctx, c, len + 1);
    hu_crisis_tiers_mode_set_for_test(-1);
}

static void test_emotional_off_is_legacy(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_OFF);
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *c = emotional_context("i can't go on like this", &len);
    HU_ASSERT_NOT_NULL(c);
    HU_ASSERT_STR_CONTAINS(c, "SAFETY");
    alloc.free(alloc.ctx, c, len + 1);
    len = 0;
    HU_ASSERT_NULL(emotional_context("i wanna kms", &len));
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* ── The inbound crisis log line carries a tag, never the handle or text ── */

static char g_cap[8][512];
static int g_cap_n;
static void cap_event(void *ctx, const hu_observer_event_t *ev) {
    (void)ctx;
    if (ev->tag == HU_OBSERVER_EVENT_ERR && g_cap_n < 8 && ev->data.err.message) {
        strncpy(g_cap[g_cap_n], ev->data.err.message, sizeof(g_cap[0]) - 1);
        g_cap[g_cap_n][sizeof(g_cap[0]) - 1] = '\0';
        g_cap_n++;
    }
}
static const hu_observer_vtable_t CAP_VT = {.record_event = cap_event};

static void test_daemon_crisis_log_has_tag_not_handle_or_text(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_log_content_set_for_test(0);
    hu_observer_t obs = {.ctx = NULL, .vtable = &CAP_VT};
    g_cap_n = 0;
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "i want to kill myself";
    const char *h = "+15551234567";
    HU_ASSERT_EQ(hu_daemon_inbound_crisis_tier(&alloc, t, strlen(t), h, strlen(h), &obs),
                 HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(g_cap_n, 1);
    HU_ASSERT_STR_CONTAINS(g_cap[0], "tier=explicit");
    HU_ASSERT_STR_CONTAINS(g_cap[0], "contact=#");
    HU_ASSERT_STR_NOT_CONTAINS(g_cap[0], "555");
    HU_ASSERT_STR_NOT_CONTAINS(g_cap[0], "kill");
    hu_log_content_set_for_test(-1);
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* SHADOW: legacy acts (any self_harm = crisis), one aggregate line names both. */
static void test_daemon_crisis_shadow_logs_both_tiers(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_SHADOW);
    hu_observer_t obs = {.ctx = NULL, .vtable = &CAP_VT};
    g_cap_n = 0;
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "what's the point";
    HU_ASSERT_EQ(hu_daemon_inbound_crisis_tier(&alloc, t, strlen(t), "x", 1, &obs),
                 HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_GE(g_cap_n, 1);
    HU_ASSERT_STR_CONTAINS(g_cap[0], "[HU_CRISIS_TIERS shadow] legacy=explicit tier=low changed=1");
    HU_ASSERT_STR_NOT_CONTAINS(g_cap[0], "point");
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* ── Log redaction (DEF-12) ────────────────────────────────────────────── */

static void test_log_who_is_tag_by_default(void) {
    hu_log_content_set_for_test(0);
    const char *h = "+15551234567";
    const char *w = HU_LOG_WHO(h, strlen(h));
    HU_ASSERT_STR_NOT_CONTAINS(w, "555");
    HU_ASSERT_EQ(w[0], '#');
    HU_ASSERT_EQ(strlen(w), 13u);
    /* stable: same handle, same tag; different handle, different tag */
    char a[32], b[32];
    hu_log_who(h, strlen(h), a, sizeof(a));
    hu_log_who("+15551234568", 12, b, sizeof(b));
    HU_ASSERT_STR_EQ(a, w);
    HU_ASSERT_TRUE(strcmp(a, b) != 0);
    hu_log_content_set_for_test(-1);
}

static void test_log_text_is_length_by_default(void) {
    hu_log_content_set_for_test(0);
    const char *t = "meet me at the bar at 9";
    const char *r = HU_LOG_TEXT(t, strlen(t), 60);
    HU_ASSERT_STR_NOT_CONTAINS(r, "bar");
    HU_ASSERT_STR_EQ(r, "<23 chars>");
    hu_log_content_set_for_test(-1);
}

static void test_log_content_flag_shows_text(void) {
    hu_log_content_set_for_test(1);
    const char *t = "meet me at the bar at 9";
    HU_ASSERT_STR_EQ(HU_LOG_TEXT(t, strlen(t), 7), "meet me");
    HU_ASSERT_STR_EQ(HU_LOG_WHO("+15551234567", 12), "+15551234567");
    hu_log_content_set_for_test(-1);
}

/* HU_DEBUG (on in prod) must not turn content logging on. */
static void test_log_content_ignores_hu_debug(void) {
    hu_log_content_set_for_test(-1);
    const char *prev = getenv("HU_LOG_CONTENT");
    unsetenv("HU_LOG_CONTENT");
    setenv("HU_DEBUG", "1", 1);
    HU_ASSERT_FALSE(hu_log_content_enabled());
    setenv("HU_LOG_CONTENT", "1", 1);
    HU_ASSERT_TRUE(hu_log_content_enabled());
    unsetenv("HU_DEBUG");
    if (prev)
        setenv("HU_LOG_CONTENT", prev, 1);
    else
        unsetenv("HU_LOG_CONTENT");
}

/* ── Round 2 (review of #596) ─────────────────────────────────────────── */

/* CRITICAL 1: a third-person pronoun before a self-object phrase is not the
 * subject of the harm — "they'd be better off without me" is about the sender. */
static void test_self_harm_self_object_never_demoted(void) {
    HU_ASSERT_EQ(tier_of("they'd be better off without me"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("they would all be better off without me"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("my wife would be better off without me"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("he'd be better off without me"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("she said i should just end my life"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("my mom thinks i want to kms"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("they told me to take my own life"), HU_SELF_HARM_EXPLICIT);
}

/* CRITICAL 2: phrasings that used to be NONE. */
static void test_self_harm_hurt_and_cut_myself(void) {
    HU_ASSERT_EQ(tier_of("i'm going to hurt myself"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i want to hurt myself"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i've been cutting myself again"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("cut myself"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("hurt myself"), HU_SELF_HARM_LOW);
    HU_ASSERT_EQ(tier_of("i cut myself shaving lol"), HU_SELF_HARM_LOW);
}

static void test_self_harm_live_and_here_phrasings(void) {
    HU_ASSERT_EQ(tier_of("no reason to live"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i do not want to live"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i don't wanna live anymore"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("dont wanna be here anymore"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("don't want to be here"), HU_SELF_HARM_LOW);
    HU_ASSERT_EQ(tier_of("i just want to disappear"), HU_SELF_HARM_LOW);
    HU_ASSERT_EQ(tier_of("i can't do this anymore"), HU_SELF_HARM_LOW);
}

/* Morphology, not a growing list: "my self" joins, joined forms split, and
 * every tense of kill + myself is one rule. */
static void test_self_harm_kill_morphology(void) {
    HU_ASSERT_EQ(tier_of("i want to kill my self"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("killmyself"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i should have killed myself"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("kills myself"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i almost killed myself on the stairs"), HU_SELF_HARM_LOW);
}

static void test_self_harm_end_it_needs_self_context(void) {
    HU_ASSERT_EQ(tier_of("i just want to end it"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("i'm going to end it tonight"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("let's end it here and pick up tomorrow"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("i'm going to end it with her"), HU_SELF_HARM_NONE);
}

/* Minors. */
static void test_self_harm_suicide_needs_personal_context(void) {
    HU_ASSERT_EQ(tier_of("watching suicide squad tonight"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("the suicide hotline number is 988"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("i've been thinking about suicide"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("suicide"), HU_SELF_HARM_LOW);
}

static void test_self_harm_third_person_reflexive_with_tried(void) {
    HU_ASSERT_EQ(tier_of("he tried to unalive himself"), HU_SELF_HARM_THIRD_PERSON);
    HU_ASSERT_EQ(tier_of("she tried to kill herself last year"), HU_SELF_HARM_THIRD_PERSON);
}

static void test_self_harm_kms_is_not_kilometres(void) {
    HU_ASSERT_EQ(tier_of("only a few kms left"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("ran 10 kms"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("ugh kms"), HU_SELF_HARM_EXPLICIT);
}

static void test_self_harm_confusable_apostrophes(void) {
    /* U+02BC and U+FF07 */
    HU_ASSERT_EQ(tier_of("I don\xca\xbct want to live"), HU_SELF_HARM_EXPLICIT);
    HU_ASSERT_EQ(tier_of("what\xef\xbc\x87s the point"), HU_SELF_HARM_LOW);
}

/* Ruling: LOW needs personal-despair context, so a meeting gripe is NONE. */
static void test_self_harm_low_needs_despair_context(void) {
    HU_ASSERT_EQ(tier_of("what's the point of this meeting"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("what's the point of this?"), HU_SELF_HARM_LOW);
    HU_ASSERT_EQ(tier_of("honestly what's the point anymore"), HU_SELF_HARM_LOW);
    HU_ASSERT_EQ(tier_of("what's the point of even trying"), HU_SELF_HARM_LOW);
    HU_ASSERT_EQ(tier_of("i can't go on like this"), HU_SELF_HARM_LOW);
    HU_ASSERT_EQ(tier_of("i can't go on the trip"), HU_SELF_HARM_NONE);
}

/* Quoting the contact is not a self-harm statement by the speaker. */
static void test_self_harm_quoting_you_is_not_actionable(void) {
    HU_ASSERT_EQ(tier_of("when you say you want to die, i'm right here"), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("you said you wanted to kill yourself, are you safe?"), HU_SELF_HARM_NONE);
}

/* CRITICAL 3: the reply side. */
static const char k_good_reply[] =
    "i'm really glad you reached out. i'm here. please call or text 988 Suicide & Crisis Lifeline "
    "(call/text 988) if it gets worse tonight";

static void test_reply_with_988_line_is_not_self_harm(void) {
    HU_ASSERT_EQ(tier_of(k_good_reply), HU_SELF_HARM_NONE);
    HU_ASSERT_EQ(tier_of("Crisis Text Line (text HOME to 741741), 988 Suicide & Crisis "
                         "Lifeline"),
                 HU_SELF_HARM_NONE);
}

static void test_reply_resources_keyed_to_inbound_only(void) {
    const char *in = "i want to die";
    const char *plain = "i'm here. talk to me";
    HU_ASSERT_TRUE(hu_self_harm_reply_needs_resources(in, strlen(in), plain, strlen(plain)));
    HU_ASSERT_FALSE(
        hu_self_harm_reply_needs_resources(in, strlen(in), k_good_reply, strlen(k_good_reply)));
    const char *low = "what's the point";
    HU_ASSERT_FALSE(hu_self_harm_reply_needs_resources(low, strlen(low), plain, strlen(plain)));
    /* the reply's own words never trigger it */
    const char *hi = "hey";
    HU_ASSERT_FALSE(hu_self_harm_reply_needs_resources(hi, 3, "i want to die lol", 17));
    size_t n = 0;
    HU_ASSERT_NOT_NULL(hu_self_harm_resource_line(&n));
    HU_ASSERT_GT(n, 0u);
}

static size_t count_of(const char *hay, const char *needle) {
    size_t c = 0, nl = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nl)
        c++;
    return c;
}

/* End to end over the production path: inbound "i want to die" → model reply
 * carrying the 988 line → final gates clear, nothing appended, nothing
 * blocked, no canned line. The reply is sent exactly as written. */
static void test_crisis_reply_e2e_good_reply_sent_as_is(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    const char *in = "i want to die";
    hu_self_harm_tier_t tier = hu_daemon_inbound_crisis_tier(&alloc, in, strlen(in), "x", 1, NULL);
    HU_ASSERT_EQ(tier, HU_SELF_HARM_EXPLICIT);
    size_t len = 0;
    char *reply = ctx_dup(&alloc, k_good_reply, &len);
    const char *why = NULL;
    HU_ASSERT_TRUE(hu_daemon_outbound_final_gates_clear(&alloc, reply, len, &why));
    HU_ASSERT_STR_EQ(why, "clear");
    HU_ASSERT_FALSE(hu_daemon_crisis_ensure_resources(&alloc, tier, &reply, &len));
    HU_ASSERT_FALSE(hu_daemon_reply_blocked(&alloc, reply, len, NULL));
    HU_ASSERT_STR_EQ(reply, k_good_reply);
    HU_ASSERT_STR_NOT_CONTAINS(reply, "brain fart");
    HU_ASSERT_EQ(count_of(reply, "988 Suicide"), 1u);
    alloc.free(alloc.ctx, reply, len + 1);
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* A reply that forgot the resources gets the line once, never twice. */
static void test_crisis_reply_missing_988_appended_once(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *reply = ctx_dup(&alloc, "i'm here with you. talk to me", &len);
    HU_ASSERT_TRUE(hu_daemon_crisis_ensure_resources(&alloc, HU_SELF_HARM_EXPLICIT, &reply, &len));
    HU_ASSERT_EQ(len, strlen(reply));
    HU_ASSERT_STR_CONTAINS(reply, "i'm here with you. talk to me");
    HU_ASSERT_EQ(count_of(reply, "988"), 2u); /* "988 Suicide ... (call/text 988)" */
    HU_ASSERT_FALSE(hu_daemon_crisis_ensure_resources(&alloc, HU_SELF_HARM_EXPLICIT, &reply, &len));
    HU_ASSERT_FALSE(hu_daemon_crisis_ensure_resources(&alloc, HU_SELF_HARM_LOW, &reply, &len));
    alloc.free(alloc.ctx, reply, len + 1);
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* A model reply that quotes the contact or offers 988 is never blocked; one
 * endorsing violence against others is (and nothing replaces it). */
static void test_crisis_reply_blocked_only_for_non_self_harm(void) {
    hu_crisis_tiers_mode_set_for_test(HU_GATE_LIVE);
    hu_allocator_t alloc = hu_system_allocator();
    const char *quote = "when you said you wanted to kill yourself, did you mean tonight?";
    HU_ASSERT_FALSE(hu_daemon_reply_blocked(&alloc, quote, strlen(quote), NULL));
    const char *self = "honestly i want to die too some days";
    HU_ASSERT_FALSE(hu_daemon_reply_blocked(&alloc, self, strlen(self), NULL));
    const char *viol = "you should kill them with violence";
    HU_ASSERT_TRUE(hu_daemon_reply_blocked(&alloc, viol, strlen(viol), NULL));
    hu_crisis_tiers_mode_set_for_test(-1);
}

/* IMPORTANT 4: the log tag is keyed, 48-bit, and the key lives 0600. */
static void test_log_tag_keyed_and_48_bit(void) {
    hu_log_content_set_for_test(0);
    uint8_t k1[16], k2[16];
    for (int i = 0; i < 16; i++) {
        k1[i] = (uint8_t)i;
        k2[i] = (uint8_t)(i + 1);
    }
    const char *h = "+15551234567";
    hu_log_tag_set_key_for_test(k1);
    uint64_t t1 = hu_log_contact_tag(h, strlen(h));
    char w1[32];
    hu_log_who(h, strlen(h), w1, sizeof(w1));
    hu_log_tag_set_key_for_test(k2);
    uint64_t t2 = hu_log_contact_tag(h, strlen(h));
    HU_ASSERT_TRUE(t1 != t2);          /* the key matters */
    HU_ASSERT_TRUE(t1 < (1ULL << 48)); /* 48 bits */
    HU_ASSERT_TRUE(t1 > 0xffffULL);    /* not the old 16-bit fold */
    HU_ASSERT_EQ(strlen(w1), 13u);     /* "#" + 12 hex */
    /* Standard SipHash-2-4: the reference vector for key 00..0f and the empty
     * message is 0x726fdb47dd0e0e31; the tag is its low 48 bits. */
    hu_log_tag_set_key_for_test(k1);
    HU_ASSERT_TRUE(hu_log_contact_tag("", 0) == 0xdb47dd0e0e31ULL);
    hu_log_tag_set_key_for_test(NULL);
    hu_log_content_set_for_test(-1);
}

static void test_log_tag_key_file_created_0600_and_stable(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_logtag", d, sizeof(d)));
    uint8_t a[16], b[16];
    memset(a, 0, sizeof(a));
    memset(b, 0xff, sizeof(b));
    HU_ASSERT_TRUE(hu_log_tag_key_load(d, a));
    char p[600];
    snprintf(p, sizeof(p), "%s/log_tag.key", d);
    struct stat st;
    HU_ASSERT_EQ(stat(p, &st), 0);
    HU_ASSERT_EQ((int)(st.st_mode & 0777), 0600);
    HU_ASSERT_EQ((long long)st.st_size, 16LL);
    HU_ASSERT_TRUE(hu_log_tag_key_load(d, b)); /* second load reads, not regenerates */
    HU_ASSERT_TRUE(memcmp(a, b, 16) == 0);
    uint8_t zero[16] = {0};
    HU_ASSERT_TRUE(memcmp(a, zero, 16) != 0);
    hu_test_rm_rf(d);
}

static void test_log_tag_key_rejects_bad_file(void) {
    char d[512];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu_logtag", d, sizeof(d)));
    char p[600];
    snprintf(p, sizeof(p), "%s/log_tag.key", d);
    FILE *f = fopen(p, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("short", f);
    fclose(f);
    uint8_t k[16];
    HU_ASSERT_FALSE(hu_log_tag_key_load(d, k));
    hu_test_rm_rf(d);
}

void run_self_harm_tests(void) {
    HU_TEST_SUITE("Self-harm detector (DEF-1)");
    HU_RUN_TEST(test_self_harm_kill_myself_is_explicit);
    HU_RUN_TEST(test_self_harm_want_to_die_is_explicit);
    HU_RUN_TEST(test_self_harm_wanna_die_lol_is_explicit);
    HU_RUN_TEST(test_self_harm_curly_apostrophe_matches);
    HU_RUN_TEST(test_self_harm_legacy_coded_phrases_stay_explicit);
    HU_RUN_TEST(test_self_harm_evasion_still_caught);
    HU_RUN_TEST(test_self_harm_whats_the_point_is_low);
    HU_RUN_TEST(test_self_harm_figures_of_speech_are_none);
    HU_RUN_TEST(test_self_harm_negations_are_none);
    HU_RUN_TEST(test_self_harm_null_and_empty_are_none);
    HU_RUN_TEST(test_self_harm_third_person);
    HU_RUN_TEST(test_self_harm_first_person_outranks_third);
    HU_RUN_TEST(test_self_harm_long_text_tail_is_read);
    HU_RUN_TEST(test_self_harm_directives_by_tier);
    HU_RUN_TEST(test_moderation_kill_myself_is_self_harm_not_violence);
    HU_RUN_TEST(test_moderation_want_to_die_flagged);
    HU_RUN_TEST(test_moderation_violence_against_others_still_flagged);
    HU_RUN_TEST(test_moderation_third_person_not_self_harm);
    HU_RUN_TEST(test_moderation_off_is_legacy);
    HU_RUN_TEST(test_crisis_tiers_default_is_live);
    HU_RUN_TEST(test_daemon_crisis_kill_myself_gets_988_directive);
    HU_RUN_TEST(test_daemon_crisis_whats_the_point_gets_check_in_not_crisis);
    HU_RUN_TEST(test_daemon_crisis_third_person_gets_support);
    HU_RUN_TEST(test_daemon_crisis_none_is_noop);
    HU_RUN_TEST(test_daemon_crisis_off_is_byte_identical_legacy);
    HU_RUN_TEST(test_voice_gate_declines_kill_myself_inbound);
    HU_RUN_TEST(test_emotional_live_whats_the_point_is_check_in);
    HU_RUN_TEST(test_emotional_live_kill_myself_is_safety);
    HU_RUN_TEST(test_emotional_off_is_legacy);

    HU_TEST_SUITE("Log redaction (DEF-12)");
    HU_RUN_TEST(test_daemon_crisis_log_has_tag_not_handle_or_text);
    HU_RUN_TEST(test_daemon_crisis_shadow_logs_both_tiers);
    HU_RUN_TEST(test_log_who_is_tag_by_default);
    HU_RUN_TEST(test_log_text_is_length_by_default);
    HU_RUN_TEST(test_log_content_flag_shows_text);
    HU_RUN_TEST(test_log_content_ignores_hu_debug);
    HU_TEST_SUITE("Self-harm round 2 (#596 review)");
    HU_RUN_TEST(test_self_harm_self_object_never_demoted);
    HU_RUN_TEST(test_self_harm_hurt_and_cut_myself);
    HU_RUN_TEST(test_self_harm_live_and_here_phrasings);
    HU_RUN_TEST(test_self_harm_kill_morphology);
    HU_RUN_TEST(test_self_harm_end_it_needs_self_context);
    HU_RUN_TEST(test_self_harm_suicide_needs_personal_context);
    HU_RUN_TEST(test_self_harm_third_person_reflexive_with_tried);
    HU_RUN_TEST(test_self_harm_kms_is_not_kilometres);
    HU_RUN_TEST(test_self_harm_confusable_apostrophes);
    HU_RUN_TEST(test_self_harm_low_needs_despair_context);
    HU_RUN_TEST(test_self_harm_quoting_you_is_not_actionable);
    HU_RUN_TEST(test_reply_with_988_line_is_not_self_harm);
    HU_RUN_TEST(test_reply_resources_keyed_to_inbound_only);
    HU_RUN_TEST(test_crisis_reply_e2e_good_reply_sent_as_is);
    HU_RUN_TEST(test_crisis_reply_missing_988_appended_once);
    HU_RUN_TEST(test_crisis_reply_blocked_only_for_non_self_harm);
    HU_RUN_TEST(test_log_tag_keyed_and_48_bit);
    HU_RUN_TEST(test_log_tag_key_file_created_0600_and_stable);
    HU_RUN_TEST(test_log_tag_key_rejects_bad_file);
}
