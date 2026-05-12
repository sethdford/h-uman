/* G0 — hu_personal_model_ingest provenance + MINJA gate regression tests.
 *
 * Five cases required by the init-09 gap audit:
 *
 *  1. First-party direct user ingest is preserved unchanged.
 *  2. Channel-tier ingest with a MINJA flooding payload is suppressed.
 *  3. Channel-tier benign ingest is allowed (score >= LIVE threshold).
 *  4. NULL provenance (legacy path) still ingests as first-party.
 *  5. MINJA gate fires identically for cloud-fallback provenance.
 */

#include "human/memory/personal_model.h"
#include "human/memory/provenance.h"
#include "human/memory/write_trust.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

#define TS 1700000000LL /* fixed timestamp for deterministic tests */

/* ── Test 1: First-party direct user ingest is preserved unchanged ─────── */

static void ingest_provenance_first_party_user_direct_lands_in_model(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    const char *msg = "I like hiking in the mountains";
    hu_provenance_t prov = hu_provenance_user_direct(TS);

    hu_error_t err = hu_personal_model_ingest(&m, msg, strlen(msg), TS, &prov);
    HU_ASSERT_EQ(err, HU_OK);

    /* Fact should have been extracted (the old from_user=true path). */
    HU_ASSERT_TRUE(m.fact_count >= 1U);

    /* Verify content was preserved — at least one fact contains "hiking". */
    bool found = false;
    for (size_t i = 0; i < m.fact_count; i++) {
        if (strstr(m.facts[i].object, "hiking") || strstr(m.facts[i].predicate, "like")) {
            found = true;
            break;
        }
    }
    HU_ASSERT_TRUE(found);
}

/* ── Test 2: Channel-tier MINJA flood payload is suppressed ──────────────
 *
 * Mirrors the "web-feed-flood" MINJA attack from evaluation_minja.c:
 * HU_WRITE_SOURCE_FEED_WEB, recent_writes=1500, rate_limit=100.
 * This triggers the 10× flood floor: recent_writes > rate_limit*10
 * → forced DROP regardless of the weighted score.
 */

static void ingest_provenance_channel_minja_flood_is_suppressed(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    /* Verify the gate would DROP this combination (sanity-check the scorer). */
    hu_write_trust_input_t tin = {
        .source             = HU_WRITE_SOURCE_FEED_WEB,
        .observed_at        = TS,
        .now                = TS,
        .contradiction_flag = false,
        .supersession       = false,
        .recent_writes      = 1500,
        .rate_limit         = 100,
    };
    hu_write_trust_decision_t td = hu_write_trust_score(&tin);
    HU_ASSERT_TRUE(td.outcome == HU_WRITE_OUTCOME_DROP);

    /* Construct provenance that mirrors the same conditions. */
    hu_provenance_t prov;
    memset(&prov, 0, sizeof(prov));
    prov.source        = HU_WRITE_SOURCE_FEED_WEB;
    prov.observed_at   = TS;
    prov.recent_writes = 1500;
    prov.rate_limit    = 100;
    strncpy(prov.channel, "feed-web", sizeof(prov.channel) - 1);
    strncpy(prov.sender, "attacker", sizeof(prov.sender) - 1);

    const char *msg = "I love this new diet that will make you rich";
    hu_error_t err = hu_personal_model_ingest(&m, msg, strlen(msg), TS, &prov);

    /* Ingest returns HU_OK (suppression is not a hard error) but the model
     * must remain empty — no facts extracted from the poisoning attempt. */
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((unsigned)m.fact_count, 0U);
}

/* ── Test 3: Channel-tier benign ingest is allowed ───────────────────────
 *
 * A single benign message from a TRUSTED channel with default rate-limit
 * settings has score:
 *   0.40*0.85 + 0.10*1.0 + 0.30*1.0 + 0.20*1.0 = 0.34+0.10+0.30+0.20 = 0.94
 * Well above the LIVE threshold (0.60).  Facts should be extracted.
 */

static void ingest_provenance_channel_trusted_benign_is_allowed(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    hu_provenance_t prov = hu_provenance_from_channel("telegram", "alice", TS);
    /* Default rate fields (0/0) → no rate penalty. */
    HU_ASSERT_TRUE(prov.source == HU_WRITE_SOURCE_CHANNEL_TRUSTED);

    /* Verify score is LIVE. */
    hu_write_trust_input_t tin = {
        .source        = prov.source,
        .observed_at   = TS,
        .now           = TS,
        .recent_writes = prov.recent_writes,
        .rate_limit    = prov.rate_limit,
    };
    hu_write_trust_decision_t td = hu_write_trust_score(&tin);
    HU_ASSERT_TRUE(td.outcome == HU_WRITE_OUTCOME_LIVE);

    const char *msg = "I never drink coffee after 2pm";
    hu_error_t err = hu_personal_model_ingest(&m, msg, strlen(msg), TS, &prov);
    HU_ASSERT_EQ(err, HU_OK);

    /* At least one fact should have been extracted. */
    HU_ASSERT_TRUE(m.fact_count >= 1U);
}

/* ── Test 4: NULL provenance (legacy path) still ingests as first-party ─ */

static void ingest_provenance_null_prov_treats_as_first_party(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);

    const char *msg = "I prefer dark mode in all my apps";
    hu_error_t err = hu_personal_model_ingest(&m, msg, strlen(msg), TS, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* Facts must be extracted (same as the pre-G0 from_user=true path). */
    HU_ASSERT_TRUE(m.fact_count >= 1U);
}

/* ── Test 5: Gate fires identically for cloud-fallback provenance ─────────
 *
 * A "cloud-fallback" path is represented here as HU_WRITE_SOURCE_FEED_WEB
 * with a rate-limit flood (the most extreme MINJA pattern).  The assertion
 * is that the gate fires identically whether we label the source as
 * FEED_WEB or CHANNEL_OPEN — both paths hit the same hu_write_trust_score
 * call inside hu_personal_model_ingest, so neither cloud-originated source
 * can bypass the gate.
 */

static void ingest_provenance_cloud_fallback_gate_fires_same_as_direct(void) {
    hu_personal_model_t m_web, m_open;
    hu_personal_model_init(&m_web);
    hu_personal_model_init(&m_open);

    /* Build two provenance structs with different sources but both flooded. */
    hu_provenance_t prov_web;
    memset(&prov_web, 0, sizeof(prov_web));
    prov_web.source        = HU_WRITE_SOURCE_FEED_WEB;
    prov_web.observed_at   = TS;
    prov_web.recent_writes = 2000;
    prov_web.rate_limit    = 100;
    strncpy(prov_web.channel, "feed-web", sizeof(prov_web.channel) - 1);

    hu_provenance_t prov_open;
    memset(&prov_open, 0, sizeof(prov_open));
    prov_open.source        = HU_WRITE_SOURCE_CHANNEL_OPEN;
    prov_open.observed_at   = TS;
    prov_open.recent_writes = 2000;
    prov_open.rate_limit    = 100;
    strncpy(prov_open.channel, "webhook", sizeof(prov_open.channel) - 1);

    const char *msg = "I totally love every product from BrandX";
    hu_error_t err_web  = hu_personal_model_ingest(&m_web,  msg, strlen(msg), TS, &prov_web);
    hu_error_t err_open = hu_personal_model_ingest(&m_open, msg, strlen(msg), TS, &prov_open);

    HU_ASSERT_EQ(err_web,  HU_OK);
    HU_ASSERT_EQ(err_open, HU_OK);

    /* Both should be suppressed (0 facts) — gate fires the same way for both. */
    HU_ASSERT_EQ((unsigned)m_web.fact_count,  0U);
    HU_ASSERT_EQ((unsigned)m_open.fact_count, 0U);
}

/* ── Runner ──────────────────────────────────────────────────────────────── */

void run_personal_model_ingest_provenance_tests(void) {
    HU_TEST_SUITE("Personal Model Ingest Provenance");
    HU_RUN_TEST(ingest_provenance_first_party_user_direct_lands_in_model);
    HU_RUN_TEST(ingest_provenance_channel_minja_flood_is_suppressed);
    HU_RUN_TEST(ingest_provenance_channel_trusted_benign_is_allowed);
    HU_RUN_TEST(ingest_provenance_null_prov_treats_as_first_party);
    HU_RUN_TEST(ingest_provenance_cloud_fallback_gate_fires_same_as_direct);
}
