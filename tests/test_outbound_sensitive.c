/* test_outbound_sensitive.c — owner's-own-data disclosure gate contract.
 *
 * The 2026-07-29 audit found the outbound path had no check for the OWNER's
 * sensitive data leaving. These tests pin both directions of the contract, and
 * the false-positive direction matters MORE than the blocking direction: Seth
 * legitimately tells people his city, employer, and kids' names constantly, so
 * a gate that blocks those breaks the product worse than the leak it prevents.
 * Several tests below use his REAL corpus lines as must-not-block fixtures.
 *
 * Every test name is a claim that fails if the claim is false — no
 * `count >= 0` tautologies (.claude/rules/tests-that-pin-bugs.md).
 *
 * Units under test:
 *   src/agent/outbound/sensitive.c        — predicate + pipeline stage + gate
 *   src/agent/outbound/sensitive_config.c — config -> protected-set adapter
 *   src/daemon/daemon_outbound_wiring.c   — startup registration + teardown
 *   src/config/config_parse.c             — the `privacy` block parse
 */

#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#include "human/agent/outbound_pipeline.h"
#include "human/agent/outbound_sensitive.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/daemon/daemon_outbound_wiring.h"

extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_sensitive;

static hu_allocator_t *test_alloc(void) {
    static hu_allocator_t a;
    static int init = 0;
    if (!init) {
        a = hu_system_allocator();
        init = 1;
    }
    return &a;
}

/* ── The owner's declared protected set (mirrors the real persona) ─────── */

/* The street address that leaked verbatim into a rating sheet, the employer
 * and city from persona core.identity, and the kids' names. */
static const hu_sensitive_value_t owner_values[] = {
    {"4341 34th St S", 14, HU_SENSITIVE_TIER_NEVER_SEND, HU_SENSITIVE_CAT_STREET_ADDRESS},
    {"Vanguard", 8, HU_SENSITIVE_TIER_TRUST_GATED, HU_SENSITIVE_CAT_EMPLOYER},
    {"St Petersburg", 13, HU_SENSITIVE_TIER_TRUST_GATED, HU_SENSITIVE_CAT_CITY},
    {"Annette", 7, HU_SENSITIVE_TIER_TRUST_GATED, HU_SENSITIVE_CAT_FAMILY_NAME},
    {"Ford", 4, HU_SENSITIVE_TIER_TRUST_GATED, HU_SENSITIVE_CAT_FAMILY_NAME},
};
static const hu_sensitive_set_t owner_set = {owner_values,
                                             sizeof(owner_values) / sizeof(owner_values[0])};

static const hu_sensitive_set_t empty_set = {NULL, 0};

static int owner_provider(void *ud, const hu_sensitive_set_t **out) {
    (void)ud;
    *out = &owner_set;
    return 0;
}

static int empty_provider(void *ud, const hu_sensitive_set_t **out) {
    (void)ud;
    *out = &empty_set;
    return 0;
}

/* ----------------------------------------------------------------- */
/* Pure predicate — NEVER_SEND street address                         */
/* ----------------------------------------------------------------- */

static void test_scan_declared_street_address_is_never_send(void) {
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan("sure, it's 4341 34th St S, buzz 652 when you're here", 52,
                                     &owner_set, &f));
    HU_ASSERT_EQ((int)f.tier, (int)HU_SENSITIVE_TIER_NEVER_SEND);
    HU_ASSERT_EQ((int)f.category, (int)HU_SENSITIVE_CAT_STREET_ADDRESS);
}

/* The declared form is the SHORT one; the reply carries the long form with a
 * unit designator. Both must reduce to the same core. */
static void test_scan_address_with_apt_suffix_still_matches_short_declaration(void) {
    const char *reply = "4341 34TH ST S APT 652";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), &owner_set, &f));
    HU_ASSERT_EQ((int)f.category, (int)HU_SENSITIVE_CAT_STREET_ADDRESS);
}

/* Punctuation and case differences must not defeat the match. */
static void test_scan_address_normalizes_case_and_punctuation(void) {
    const char *reply = "i'm at 4341 34th st. s -- come up";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), &owner_set, &f));
    HU_ASSERT_EQ((int)f.tier, (int)HU_SENSITIVE_TIER_NEVER_SEND);
}

/* ----------------------------------------------------------------- */
/* FALSE-POSITIVE GUARDS — the failure mode that breaks the product   */
/* ----------------------------------------------------------------- */

/* A third-party address is not the owner's data. Shape-only detection would
 * have blocked this, which is why the address rule requires a value match. */
static void test_scan_third_party_address_is_allowed(void) {
    const char *reply = "meet me at 200 Central Ave around 7";
    HU_ASSERT_FALSE(hu_sensitive_scan(reply, strlen(reply), &owner_set, NULL));
}

/* Real corpus line. "waterfront place" has a street-type token but no house
 * number, so it is not an address, and the city is trust-gated (not blocked). */
static void test_scan_real_corpus_city_line_is_not_never_send(void) {
    const char *reply = "st. petersburg, waterfront place on tampa bay";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), &owner_set, &f));
    HU_ASSERT_EQ((int)f.tier, (int)HU_SENSITIVE_TIER_TRUST_GATED);
    HU_ASSERT_EQ((int)f.category, (int)HU_SENSITIVE_CAT_CITY);
}

/* Real corpus line — the owner introduces himself by employer routinely. */
static void test_scan_real_corpus_employer_line_is_trust_gated_not_blocked(void) {
    const char *reply = "It's Seth, from Vanguard";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), &owner_set, &f));
    HU_ASSERT_EQ((int)f.tier, (int)HU_SENSITIVE_TIER_TRUST_GATED);
    HU_ASSERT_EQ((int)f.category, (int)HU_SENSITIVE_CAT_EMPLOYER);
}

/* Word-boundary matching, per ~/.claude/rules/substring-classifier-pitfalls.md.
 * A protected "Ford" must NOT fire on "afford"; "Vanguard" must not fire on
 * "vanguards". These are the exact substring traps that rule documents. */
static void test_scan_resembling_words_do_not_match(void) {
    const char *afford = "honestly we can't afford that right now";
    HU_ASSERT_FALSE(hu_sensitive_scan(afford, strlen(afford), &owner_set, NULL));

    const char *vanguards = "they were the vanguards of the whole thing";
    HU_ASSERT_FALSE(hu_sensitive_scan(vanguards, strlen(vanguards), &owner_set, NULL));
}

/* A near-miss house number must not match: "14341" is not "4341". */
static void test_scan_different_house_number_does_not_match(void) {
    const char *reply = "it's 14341 34th St S";
    HU_ASSERT_FALSE(hu_sensitive_scan(reply, strlen(reply), &owner_set, NULL));
}

/* PROVES the S2 keyword/pattern layer was NOT reused. hu_sensitivity_classify_message
 * flags any 10-15 digit run as a phone number and any x@y.z as email — correct
 * for routing an inbound message to a local model, catastrophic as an outbound
 * block, because the owner sends both constantly. */
static void test_scan_phone_number_is_not_a_disclosure(void) {
    const char *reply = "call me at 727-555-0143 when you land";
    HU_ASSERT_FALSE(hu_sensitive_scan(reply, strlen(reply), &owner_set, NULL));
}

static void test_scan_email_address_is_not_a_disclosure(void) {
    const char *reply = "send it to seth@example.com and i'll look tonight";
    HU_ASSERT_FALSE(hu_sensitive_scan(reply, strlen(reply), &owner_set, NULL));
}

/* S2 keywords like "salary" appear in ordinary conversation. */
static void test_scan_sensitive_sounding_keywords_alone_are_allowed(void) {
    const char *reply = "we talked about salary and my diagnosis came back fine";
    HU_ASSERT_FALSE(hu_sensitive_scan(reply, strlen(reply), &owner_set, NULL));
}

static void test_scan_ordinary_message_is_allowed(void) {
    const char *reply = "yeah sounds good, see you saturday";
    HU_ASSERT_FALSE(hu_sensitive_scan(reply, strlen(reply), &owner_set, NULL));
}

/* ----------------------------------------------------------------- */
/* Hard-secret SHAPES — work with NO declared values                  */
/* ----------------------------------------------------------------- */

static void test_scan_card_number_blocks_with_empty_set(void) {
    const char *reply = "card is 4111 1111 1111 1111";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), &empty_set, &f));
    HU_ASSERT_EQ((int)f.tier, (int)HU_SENSITIVE_TIER_NEVER_SEND);
    HU_ASSERT_EQ((int)f.category, (int)HU_SENSITIVE_CAT_HARD_SECRET);
    HU_ASSERT_EQ((int)f.secret_kind, (int)HU_HARD_SECRET_CARD);
}

static void test_scan_ssn_blocks_with_null_set(void) {
    const char *reply = "ssn 123-45-6789";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), NULL, &f));
    HU_ASSERT_EQ((int)f.secret_kind, (int)HU_HARD_SECRET_SSN);
}

static void test_scan_api_token_blocks(void) {
    const char *reply = "use sk-abcdefghijklmnopqrstuvwx to auth";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), NULL, &f));
    HU_ASSERT_EQ((int)f.secret_kind, (int)HU_HARD_SECRET_API_TOKEN);
}

static void test_scan_private_key_header_blocks(void) {
    const char *reply = "-----BEGIN RSA PRIVATE KEY-----\nMIIBOgIB";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), NULL, &f));
    HU_ASSERT_EQ((int)f.secret_kind, (int)HU_HARD_SECRET_PRIVATE_KEY);
}

/* "sk-" is a prefix of ordinary words; a bare occurrence must not fire. */
static void test_scan_short_sk_prefix_is_not_a_token(void) {
    const char *reply = "ask-me later ok";
    HU_ASSERT_FALSE(hu_sensitive_scan(reply, strlen(reply), NULL, NULL));
}

static void test_scan_empty_and_null_text_are_clean(void) {
    HU_ASSERT_FALSE(hu_sensitive_scan(NULL, 0, &owner_set, NULL));
    HU_ASSERT_FALSE(hu_sensitive_scan("", 0, &owner_set, NULL));
}

/* ----------------------------------------------------------------- */
/* Tier ordering + long-message windowing                             */
/* ----------------------------------------------------------------- */

/* When both tiers are present the caller must see the severe one, else a
 * caller branching on tier alone would under-react. */
static void test_scan_never_send_outranks_trust_gated(void) {
    const char *reply = "i'm at 4341 34th St S here in St Petersburg";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), &owner_set, &f));
    HU_ASSERT_EQ((int)f.tier, (int)HU_SENSITIVE_TIER_NEVER_SEND);
}

/* The scan uses a fixed buffer, so it must WINDOW rather than truncate. A
 * disclosure past the first window would otherwise be silently unchecked —
 * the "stopped measuring but still reported clean" failure shape. */
static void test_scan_finds_disclosure_beyond_first_window(void) {
    static char big[9000];
    memset(big, 'a', sizeof(big));
    /* Fill with plausible words so normalization has separators to work with. */
    for (size_t i = 8; i < sizeof(big); i += 8)
        big[i] = ' ';
    const char *tail = " and my place is 4341 34th St S ok";
    size_t tlen = strlen(tail);
    memcpy(big + sizeof(big) - tlen - 1, tail, tlen);
    big[sizeof(big) - 1] = '\0';

    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(big, strlen(big), &owner_set, &f));
    HU_ASSERT_EQ((int)f.category, (int)HU_SENSITIVE_CAT_STREET_ADDRESS);
}

/* ----------------------------------------------------------------- */
/* Normalization + address-core contracts                             */
/* ----------------------------------------------------------------- */

static void test_normalize_lowercases_and_collapses_separators(void) {
    char buf[64];
    size_t n = hu_sensitive_normalize("Hello,   WORLD!!  ok", 20, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "hello world ok");
    HU_ASSERT_EQ(n, strlen("hello world ok"));
}

/* Digit-group separators are dropped so "$1,250,000" and "1250000" agree. */
static void test_normalize_drops_digit_group_separators(void) {
    char buf[64];
    hu_sensitive_normalize("$1,250,000", 10, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "1250000");
}

static void test_normalize_emits_no_leading_or_trailing_space(void) {
    char buf[64];
    hu_sensitive_normalize("  ...hi there!  ", 16, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "hi there");
}

static void test_address_core_drops_unit_designator(void) {
    char buf[64];
    size_t n = hu_sensitive_address_core("4341 34TH ST S APT 652", 22, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "4341 34th st s");
    HU_ASSERT_EQ(n, strlen("4341 34th st s"));
}

static void test_address_core_keeps_trailing_directional(void) {
    char buf[64];
    hu_sensitive_address_core("4341 34th St N", 14, buf, sizeof(buf));
    /* "N" and "S" are different streets — the directional is part of identity. */
    HU_ASSERT_STR_EQ(buf, "4341 34th st n");
}

/* A value with no house number has no address shape. The scan SKIPS such a
 * value rather than falling back to substring matching — otherwise a
 * mis-declared "waterfront place" would fire on the owner's real messages. */
static void test_address_core_returns_zero_without_house_number(void) {
    char buf[64];
    HU_ASSERT_EQ(hu_sensitive_address_core("waterfront place on tampa bay", 29, buf, sizeof(buf)),
                 (size_t)0);
}

static void test_address_core_returns_zero_for_plain_words(void) {
    char buf[64];
    HU_ASSERT_EQ(hu_sensitive_address_core("Vanguard", 8, buf, sizeof(buf)), (size_t)0);
}

static void test_mis_declared_address_value_is_skipped_not_substring_matched(void) {
    static const hu_sensitive_value_t bad[] = {
        {"waterfront place", 16, HU_SENSITIVE_TIER_NEVER_SEND, HU_SENSITIVE_CAT_STREET_ADDRESS},
    };
    static const hu_sensitive_set_t bad_set = {bad, 1};
    const char *reply = "st. petersburg, waterfront place on tampa bay";
    HU_ASSERT_FALSE(hu_sensitive_scan(reply, strlen(reply), &bad_set, NULL));
}

/* ----------------------------------------------------------------- */
/* Tier defaults                                                      */
/* ----------------------------------------------------------------- */

static void test_category_default_tiers_match_doctrine(void) {
    HU_ASSERT_EQ((int)hu_sensitive_category_default_tier(HU_SENSITIVE_CAT_STREET_ADDRESS),
                 (int)HU_SENSITIVE_TIER_NEVER_SEND);
    HU_ASSERT_EQ((int)hu_sensitive_category_default_tier(HU_SENSITIVE_CAT_HARD_SECRET),
                 (int)HU_SENSITIVE_TIER_NEVER_SEND);
    HU_ASSERT_EQ((int)hu_sensitive_category_default_tier(HU_SENSITIVE_CAT_EMPLOYER),
                 (int)HU_SENSITIVE_TIER_TRUST_GATED);
    HU_ASSERT_EQ((int)hu_sensitive_category_default_tier(HU_SENSITIVE_CAT_CITY),
                 (int)HU_SENSITIVE_TIER_TRUST_GATED);
    HU_ASSERT_EQ((int)hu_sensitive_category_default_tier(HU_SENSITIVE_CAT_FAMILY_NAME),
                 (int)HU_SENSITIVE_TIER_TRUST_GATED);
    HU_ASSERT_EQ((int)hu_sensitive_category_default_tier(HU_SENSITIVE_CAT_FINANCIAL),
                 (int)HU_SENSITIVE_TIER_TRUST_GATED);
}

/* A value that declares no explicit tier inherits its category's tier. */
static void test_value_with_unset_tier_inherits_category_tier(void) {
    static const hu_sensitive_value_t untiered[] = {
        {"4341 34th St S", 14, HU_SENSITIVE_TIER_NONE, HU_SENSITIVE_CAT_STREET_ADDRESS},
    };
    static const hu_sensitive_set_t s = {untiered, 1};
    const char *reply = "4341 34th St S";
    hu_sensitive_finding_t f;
    HU_ASSERT_TRUE(hu_sensitive_scan(reply, strlen(reply), &s, &f));
    HU_ASSERT_EQ((int)f.tier, (int)HU_SENSITIVE_TIER_NEVER_SEND);
}

/* ----------------------------------------------------------------- */
/* Pipeline stage — gate ladder                                       */
/* ----------------------------------------------------------------- */

static hu_outbound_verdict_t run_stage(const char *content, int regenerate_budget) {
    hu_outbound_message_t msg = {0};
    msg.content = (char *)content;
    msg.content_len = strlen(content);

    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    ctx.path = HU_OUTBOUND_PATH_REACTIVE;
    ctx.regenerate_budget = regenerate_budget;

    return hu_outbound_pipeline_stage_sensitive.run(&hu_outbound_pipeline_stage_sensitive, &msg,
                                                    &ctx);
}

/* The gate ships DEFAULT SHADOW so a false-positive rate can be measured on
 * live traffic before any message is altered. */
static void test_default_mode_is_shadow(void) {
    unsetenv("HU_OUTBOUND_SENSITIVE");
    hu_outbound_sensitive_set_mode_for_test(-1); /* force re-read of env */
    HU_ASSERT_EQ((int)hu_outbound_sensitive_mode(), (int)HU_SENSITIVE_MODE_SHADOW);
}

static void test_stage_shadow_does_not_alter_a_disclosing_message(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_SHADOW);
    hu_outbound_verdict_t v = run_stage("i'm at 4341 34th St S", 1);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_SEND);
    HU_ASSERT_NULL(v.replacement);
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

static void test_stage_off_does_not_alter_a_disclosing_message(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_OFF);
    hu_outbound_verdict_t v = run_stage("i'm at 4341 34th St S", 1);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_SEND);
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

/* LIVE + budget available → regenerate once with an explicit non-disclosure
 * instruction (not a silent truncation). */
static void test_stage_live_regenerates_with_non_disclosure_hint(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    hu_outbound_verdict_t v = run_stage("i'm at 4341 34th St S", 1);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_REGENERATE);
    HU_ASSERT_NOT_NULL(v.regenerate_hint);
    HU_ASSERT_STR_CONTAINS(v.regenerate_hint, "street address");
    HU_ASSERT_NOT_NULL(v.reason);
    HU_ASSERT_STR_CONTAINS(v.reason, "street_address");
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

/* Budget spent and it STILL discloses → coherent deflection, and crucially the
 * replacement must NOT carry the protected value. */
static void test_stage_live_deflects_when_regenerate_budget_spent(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    hu_outbound_verdict_t v = run_stage("i'm at 4341 34th St S", 0);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_REWRITE);
    HU_ASSERT_NOT_NULL(v.replacement);
    HU_ASSERT_STR_EQ(v.replacement, HU_SENSITIVE_DEFLECTION);
    HU_ASSERT_STR_NOT_CONTAINS(v.replacement, "4341");
    hu_outbound_verdict_clear(&v, test_alloc());
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

/* Tier (b) is DETECTED but not ENFORCED until a recipient-trust predicate
 * exists. This test documents that as behavior so the day trust lands, it
 * fails and forces a deliberate update. See the TODO in sensitive.c. */
static void test_stage_live_trust_gated_city_still_sends_pending_trust_signal(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    hu_outbound_verdict_t v = run_stage("yeah, St Petersburg these days", 1);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_SEND);
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

/* LIVE + a card shape must block even with no declared values at all. */
static void test_stage_live_blocks_card_with_no_declared_values(void) {
    hu_outbound_sensitive_set_provider(empty_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    hu_outbound_verdict_t v = run_stage("card is 4111 1111 1111 1111", 0);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_REWRITE);
    HU_ASSERT_STR_EQ(v.replacement, HU_SENSITIVE_DEFLECTION);
    hu_outbound_verdict_clear(&v, test_alloc());
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

static void test_stage_live_sends_clean_message(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    hu_outbound_verdict_t v = run_stage("sounds good, see you saturday", 1);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_SEND);
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

static void test_stage_empty_content_sends(void) {
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    hu_outbound_verdict_t v = run_stage("", 1);
    HU_ASSERT_EQ((int)v.kind, (int)HU_OUTBOUND_SEND);
}

/* ----------------------------------------------------------------- */
/* Integration — the stage is actually IN the pipeline                */
/* ----------------------------------------------------------------- */

/* Per ~/.claude/rules/verify-before-you-claim.md, a stage that exists but is
 * never reached is not wired. This drives the REAL pipeline for the reactive
 * path — the path that answers live inbound texts and the only one with no
 * `moderation` stage — and asserts a card number does not survive it. */
static void test_reactive_pipeline_actually_runs_the_sensitive_stage(void) {
    hu_allocator_t *alloc = test_alloc();
    hu_outbound_sensitive_set_provider(empty_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);

    hu_outbound_pipeline_t *pipeline = NULL;
    HU_ASSERT_EQ(hu_outbound_pipeline_for_path(alloc, HU_OUTBOUND_PATH_REACTIVE, &pipeline), HU_OK);
    HU_ASSERT_NOT_NULL(pipeline);

    const char *src = "card is 4111 1111 1111 1111";
    size_t slen = strlen(src);
    char *heap = (char *)alloc->alloc(alloc->ctx, slen + 1);
    HU_ASSERT_NOT_NULL(heap);
    memcpy(heap, src, slen + 1);

    hu_outbound_message_t msg = {0};
    msg.content = heap;
    msg.content_len = slen;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = alloc;
    ctx.path = HU_OUTBOUND_PATH_REACTIVE;
    ctx.regenerate_budget = 0;

    hu_outbound_verdict_t v = {0};
    HU_ASSERT_EQ(hu_outbound_pipeline_run(pipeline, &msg, &ctx, &v), HU_OK);

    /* Budget 0 → the stage rewrites to the deflection, which the pipeline
     * applies before re-entering; the card must be gone from the payload. */
    HU_ASSERT_STR_NOT_CONTAINS(msg.content, "4111");

    hu_outbound_verdict_clear(&v, alloc);
    if (msg.content)
        alloc->free(alloc->ctx, msg.content, msg.content_len + 1);
    hu_outbound_pipeline_destroy(pipeline);
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

/* ----------------------------------------------------------------- */
/* Reactive in-place path                                             */
/* ----------------------------------------------------------------- */

static void test_apply_inplace_shadow_leaves_buffer_untouched(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_SHADOW);
    char buf[128] = "i'm at 4341 34th St S";
    size_t len = strlen(buf);
    size_t out = hu_outbound_sensitive_apply_inplace(buf, len, sizeof(buf));
    HU_ASSERT_EQ(out, len);
    HU_ASSERT_STR_EQ(buf, "i'm at 4341 34th St S");
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

static void test_apply_inplace_live_replaces_with_deflection(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    char buf[128] = "i'm at 4341 34th St S";
    size_t out = hu_outbound_sensitive_apply_inplace(buf, strlen(buf), sizeof(buf));
    HU_ASSERT_STR_EQ(buf, HU_SENSITIVE_DEFLECTION);
    HU_ASSERT_EQ(out, strlen(HU_SENSITIVE_DEFLECTION));
    HU_ASSERT_STR_NOT_CONTAINS(buf, "4341");
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

/* Too small to hold the deflection → empty the buffer. Never ship a
 * half-scrubbed body. */
static void test_apply_inplace_fails_closed_when_deflection_does_not_fit(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    char buf[24] = "at 4341 34th St S";
    size_t out = hu_outbound_sensitive_apply_inplace(buf, strlen(buf), sizeof(buf));
    HU_ASSERT_EQ(out, (size_t)0);
    HU_ASSERT_EQ((int)buf[0], 0);
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

static void test_apply_inplace_live_leaves_clean_message_alone(void) {
    hu_outbound_sensitive_set_provider(owner_provider, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    char buf[128] = "sounds good, see you saturday";
    size_t len = strlen(buf);
    HU_ASSERT_EQ(hu_outbound_sensitive_apply_inplace(buf, len, sizeof(buf)), len);
    HU_ASSERT_STR_EQ(buf, "sounds good, see you saturday");
    hu_outbound_sensitive_set_provider(NULL, NULL);
}

/* No provider wired at all → hard-secret shapes still block. This is the
 * fail-open boundary: value-based checks go quiet, shape-based ones do not. */
static void test_apply_inplace_with_no_provider_still_blocks_hard_secret(void) {
    hu_outbound_sensitive_set_provider(NULL, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    char buf[128] = "ssn 123-45-6789";
    hu_outbound_sensitive_apply_inplace(buf, strlen(buf), sizeof(buf));
    HU_ASSERT_STR_EQ(buf, HU_SENSITIVE_DEFLECTION);
}

/* ...and an ordinary message is untouched with no provider. */
static void test_apply_inplace_with_no_provider_allows_ordinary_message(void) {
    hu_outbound_sensitive_set_provider(NULL, NULL);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    char buf[128] = "yeah st petersburg, on the water now";
    size_t len = strlen(buf);
    HU_ASSERT_EQ(hu_outbound_sensitive_apply_inplace(buf, len, sizeof(buf)), len);
    HU_ASSERT_STR_EQ(buf, "yeah st petersburg, on the water now");
}

/* ----------------------------------------------------------------- */
/* Config adapter — the production source of the protected set        */
/* ----------------------------------------------------------------- */

/* The stage is only as good as the data it is handed. These pin the
 * config -> set mapping so a wrong category-to-tier assignment cannot ship
 * silently (an address filed as trust-gated would never block). */
static void test_register_config_maps_categories_to_tiers(void) {
    static hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    static char *addr[] = {(char *)"4341 34th St S"};
    static char *emp[] = {(char *)"Vanguard"};
    static char *kids[] = {(char *)"Annette", (char *)"Emerson"};
    cfg.privacy.street_address = addr;
    cfg.privacy.street_address_count = 1;
    cfg.privacy.employer = emp;
    cfg.privacy.employer_count = 1;
    cfg.privacy.family_names = kids;
    cfg.privacy.family_names_count = 2;

    hu_outbound_sensitive_register_config(&cfg);
    const hu_sensitive_set_t *set = hu_outbound_sensitive_current_set();
    HU_ASSERT_NOT_NULL(set);
    HU_ASSERT_EQ(set->count, (size_t)4);

    /* The address must arrive as NEVER_SEND, the rest as TRUST_GATED. */
    int never_send = 0, trust_gated = 0;
    for (size_t i = 0; i < set->count; i++) {
        if (set->values[i].category == HU_SENSITIVE_CAT_STREET_ADDRESS) {
            HU_ASSERT_EQ((int)set->values[i].tier, (int)HU_SENSITIVE_TIER_NEVER_SEND);
            never_send++;
        } else {
            HU_ASSERT_EQ((int)set->values[i].tier, (int)HU_SENSITIVE_TIER_TRUST_GATED);
            trust_gated++;
        }
    }
    HU_ASSERT_EQ(never_send, 1);
    HU_ASSERT_EQ(trust_gated, 3);

    hu_outbound_sensitive_register_config(NULL);
}

/* End-to-end through the real config path: a config-declared address blocks. */
static void test_config_declared_address_blocks_via_registered_set(void) {
    static hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    static char *addr[] = {(char *)"4341 34th St S"};
    cfg.privacy.street_address = addr;
    cfg.privacy.street_address_count = 1;

    hu_outbound_sensitive_register_config(&cfg);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    char buf[128] = "4341 34TH ST S APT 652";
    hu_outbound_sensitive_apply_inplace(buf, strlen(buf), sizeof(buf));
    HU_ASSERT_STR_EQ(buf, HU_SENSITIVE_DEFLECTION);
    hu_outbound_sensitive_register_config(NULL);
}

static void test_register_config_null_clears_the_provider(void) {
    static hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_outbound_sensitive_register_config(&cfg);
    HU_ASSERT_NOT_NULL(hu_outbound_sensitive_current_set());
    hu_outbound_sensitive_register_config(NULL);
    HU_ASSERT_NULL(hu_outbound_sensitive_current_set());
}

/* An absent privacy block must yield an empty set, not a broken one. */
static void test_register_config_with_empty_privacy_block_yields_empty_set(void) {
    static hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_outbound_sensitive_register_config(&cfg);
    const hu_sensitive_set_t *set = hu_outbound_sensitive_current_set();
    HU_ASSERT_NOT_NULL(set);
    HU_ASSERT_EQ(set->count, (size_t)0);
    hu_outbound_sensitive_register_config(NULL);
}

/* ----------------------------------------------------------------- */
/* Config JSON parse — the first link in the chain                    */
/* ----------------------------------------------------------------- */

/* Without this the whole feature could be inert in production with every other
 * test still green: the stage works, the adapter works, and the parser quietly
 * never populates cfg->privacy. That is the exact shape of
 * .claude/rules/silent-config-gated-subsystems.md. */
static void test_privacy_block_parses_from_json(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);

    const char *json = "{\"privacy\":{"
                       "\"street_address\":[\"4341 34th St S\"],"
                       "\"employer\":[\"Vanguard\"],"
                       "\"city\":[\"St Petersburg\"],"
                       "\"family_names\":[\"Annette\",\"Emerson\",\"Edison\"],"
                       "\"financial\":[\"1250000\"]}}";
    HU_ASSERT_EQ(hu_config_parse_json(cfg, json, strlen(json)), HU_OK);

    HU_ASSERT_EQ(cfg->privacy.street_address_count, (size_t)1);
    HU_ASSERT_STR_EQ(cfg->privacy.street_address[0], "4341 34th St S");
    HU_ASSERT_EQ(cfg->privacy.employer_count, (size_t)1);
    HU_ASSERT_STR_EQ(cfg->privacy.employer[0], "Vanguard");
    HU_ASSERT_EQ(cfg->privacy.city_count, (size_t)1);
    HU_ASSERT_EQ(cfg->privacy.family_names_count, (size_t)3);
    HU_ASSERT_STR_EQ(cfg->privacy.family_names[2], "Edison");
    HU_ASSERT_EQ(cfg->privacy.financial_count, (size_t)1);

    /* ...and the parsed block drives a real block through the registered set. */
    hu_outbound_sensitive_register_config(cfg);
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_LIVE);
    char buf[128] = "come by 4341 34th St S";
    hu_outbound_sensitive_apply_inplace(buf, strlen(buf), sizeof(buf));
    HU_ASSERT_STR_EQ(buf, HU_SENSITIVE_DEFLECTION);
    hu_outbound_sensitive_register_config(NULL);

    hu_arena_destroy(arena);
    backing.free(backing.ctx, cfg, sizeof(*cfg));
}

/* An absent privacy block must leave every count at zero — not crash, not
 * inherit stale values. */
static void test_config_without_privacy_block_leaves_counts_zero(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);

    HU_ASSERT_EQ(hu_config_parse_json(cfg, "{}", 2), HU_OK);
    HU_ASSERT_EQ(cfg->privacy.street_address_count, (size_t)0);
    HU_ASSERT_EQ(cfg->privacy.employer_count, (size_t)0);
    HU_ASSERT_NULL(cfg->privacy.street_address);

    hu_arena_destroy(arena);
    backing.free(backing.ctx, cfg, sizeof(*cfg));
}

/* ----------------------------------------------------------------- */
/* Startup wiring — src/daemon/daemon_outbound_wiring.c               */
/* ----------------------------------------------------------------- */

/* The registration was moved out of daemon.c into a helper; this proves the
 * helper actually registers (a silently-no-op init would leave the gate
 * value-blind in production while every other test stayed green). */
static void test_wiring_init_registers_the_protected_set(void) {
    static hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    static char *addr[] = {(char *)"4341 34th St S"};
    cfg.privacy.street_address = addr;
    cfg.privacy.street_address_count = 1;

    hu_outbound_sensitive_set_provider(NULL, NULL);
    HU_ASSERT_NULL(hu_outbound_sensitive_current_set());

    /* NULL agent: the crosstalk half degrades on its own terms; the sensitive
     * half must still wire. */
    hu_daemon_outbound_wiring_init(&cfg, NULL);
    const hu_sensitive_set_t *set = hu_outbound_sensitive_current_set();
    HU_ASSERT_NOT_NULL(set);
    HU_ASSERT_EQ(set->count, (size_t)1);
    HU_ASSERT_EQ((int)set->values[0].category, (int)HU_SENSITIVE_CAT_STREET_ADDRESS);

    hu_daemon_outbound_wiring_teardown();
    HU_ASSERT_NULL(hu_outbound_sensitive_current_set());
}

/* Teardown must be safe when init never ran — it runs on every shutdown path. */
static void test_wiring_teardown_is_safe_without_init(void) {
    hu_daemon_outbound_wiring_teardown();
    hu_daemon_outbound_wiring_teardown();
    HU_ASSERT_NULL(hu_outbound_sensitive_current_set());
}

void run_outbound_sensitive_tests(void) {
    HU_TEST_SUITE("outbound_sensitive");

    /* Pure predicate — blocking direction */
    HU_RUN_TEST(test_scan_declared_street_address_is_never_send);
    HU_RUN_TEST(test_scan_address_with_apt_suffix_still_matches_short_declaration);
    HU_RUN_TEST(test_scan_address_normalizes_case_and_punctuation);

    /* False-positive guards */
    HU_RUN_TEST(test_scan_third_party_address_is_allowed);
    HU_RUN_TEST(test_scan_real_corpus_city_line_is_not_never_send);
    HU_RUN_TEST(test_scan_real_corpus_employer_line_is_trust_gated_not_blocked);
    HU_RUN_TEST(test_scan_resembling_words_do_not_match);
    HU_RUN_TEST(test_scan_different_house_number_does_not_match);
    HU_RUN_TEST(test_scan_phone_number_is_not_a_disclosure);
    HU_RUN_TEST(test_scan_email_address_is_not_a_disclosure);
    HU_RUN_TEST(test_scan_sensitive_sounding_keywords_alone_are_allowed);
    HU_RUN_TEST(test_scan_ordinary_message_is_allowed);

    /* Hard-secret shapes */
    HU_RUN_TEST(test_scan_card_number_blocks_with_empty_set);
    HU_RUN_TEST(test_scan_ssn_blocks_with_null_set);
    HU_RUN_TEST(test_scan_api_token_blocks);
    HU_RUN_TEST(test_scan_private_key_header_blocks);
    HU_RUN_TEST(test_scan_short_sk_prefix_is_not_a_token);
    HU_RUN_TEST(test_scan_empty_and_null_text_are_clean);

    /* Tier ordering + windowing */
    HU_RUN_TEST(test_scan_never_send_outranks_trust_gated);
    HU_RUN_TEST(test_scan_finds_disclosure_beyond_first_window);

    /* Normalization + address core */
    HU_RUN_TEST(test_normalize_lowercases_and_collapses_separators);
    HU_RUN_TEST(test_normalize_drops_digit_group_separators);
    HU_RUN_TEST(test_normalize_emits_no_leading_or_trailing_space);
    HU_RUN_TEST(test_address_core_drops_unit_designator);
    HU_RUN_TEST(test_address_core_keeps_trailing_directional);
    HU_RUN_TEST(test_address_core_returns_zero_without_house_number);
    HU_RUN_TEST(test_address_core_returns_zero_for_plain_words);
    HU_RUN_TEST(test_mis_declared_address_value_is_skipped_not_substring_matched);

    /* Tier defaults */
    HU_RUN_TEST(test_category_default_tiers_match_doctrine);
    HU_RUN_TEST(test_value_with_unset_tier_inherits_category_tier);

    /* Stage gate ladder */
    HU_RUN_TEST(test_default_mode_is_shadow);
    HU_RUN_TEST(test_stage_shadow_does_not_alter_a_disclosing_message);
    HU_RUN_TEST(test_stage_off_does_not_alter_a_disclosing_message);
    HU_RUN_TEST(test_stage_live_regenerates_with_non_disclosure_hint);
    HU_RUN_TEST(test_stage_live_deflects_when_regenerate_budget_spent);
    HU_RUN_TEST(test_stage_live_trust_gated_city_still_sends_pending_trust_signal);
    HU_RUN_TEST(test_stage_live_blocks_card_with_no_declared_values);
    HU_RUN_TEST(test_stage_live_sends_clean_message);
    HU_RUN_TEST(test_stage_empty_content_sends);

    /* Integration — proves the stage is reached */
    HU_RUN_TEST(test_reactive_pipeline_actually_runs_the_sensitive_stage);

    /* Startup wiring */
    HU_RUN_TEST(test_wiring_init_registers_the_protected_set);
    HU_RUN_TEST(test_wiring_teardown_is_safe_without_init);

    /* Config JSON parse — first link in the chain */
    HU_RUN_TEST(test_privacy_block_parses_from_json);
    HU_RUN_TEST(test_config_without_privacy_block_leaves_counts_zero);

    /* Config adapter — production source of the protected set */
    HU_RUN_TEST(test_register_config_maps_categories_to_tiers);
    HU_RUN_TEST(test_config_declared_address_blocks_via_registered_set);
    HU_RUN_TEST(test_register_config_null_clears_the_provider);
    HU_RUN_TEST(test_register_config_with_empty_privacy_block_yields_empty_set);

    /* Reactive in-place path */
    HU_RUN_TEST(test_apply_inplace_shadow_leaves_buffer_untouched);
    HU_RUN_TEST(test_apply_inplace_live_replaces_with_deflection);
    HU_RUN_TEST(test_apply_inplace_fails_closed_when_deflection_does_not_fit);
    HU_RUN_TEST(test_apply_inplace_live_leaves_clean_message_alone);
    HU_RUN_TEST(test_apply_inplace_with_no_provider_still_blocks_hard_secret);
    HU_RUN_TEST(test_apply_inplace_with_no_provider_allows_ordinary_message);

    /* Leave the process in the shipped default so later suites see SHADOW. */
    hu_outbound_sensitive_set_mode_for_test((int)HU_SENSITIVE_MODE_SHADOW);
    hu_outbound_sensitive_set_provider(NULL, NULL);
}
