/* test_molora_router — Sprint 7 US-7.8 acceptance tests.
 *
 * AC coverage:
 *   AC-7.8.1 → test_static_router_selects_channel_adapter
 *   AC-7.8.2 → test_static_router_fallback_to_default
 *   AC-7.8.3 → test_disabled_molora_no_call
 *              (compile-only guard at top of file ensures this TU is only
 *              built when HU_ENABLE_MOLORA is set; the OFF build's full test
 *              suite running cleanly is the negative evidence)
 *   AC-7.8.4 → test_router_zero_init_is_disabled
 *   AC-7.8.5 → enforced by scripts/check-molora-binary-budget.sh in CI
 *
 * Plus a normalizer table that closes risk R2 in the design doc (channel id
 * variation across the 31 channel modules: "telegram" vs "telegram:42" vs
 * "Telegram" vs " Telegram \t").
 *
 * All tests are pure-allocation tests against the router struct directly.
 * The agent-turn hook integration is exercised indirectly: the router IS the
 * unit under test; the chat-dispatch glue is one `if (path) load(path)`
 * call wrapped in `#ifdef HU_ENABLE_MOLORA`, covered by the OFF-vs-ON binary
 * regression in step 7 of the implementation. */

#include "test_framework.h"

#ifdef HU_ENABLE_MOLORA

#include "human/config.h"
#include "human/config_types.h"
#include "human/core/error.h"
#include "human/ml/molora.h"

#include <stdlib.h>
#include <string.h>

/* ── Normalizer table ─────────────────────────────────────────────────── */

static void normalizer_lowercase_pass_through(void) {
    char out[HU_MOLORA_CHANNEL_NAME_MAX];
    size_t n = hu_molora_router_normalize_channel("telegram", 8, out, sizeof(out));
    HU_ASSERT_EQ(n, 8);
    HU_ASSERT(strcmp(out, "telegram") == 0);
}

static void normalizer_strips_colon_suffix(void) {
    char out[HU_MOLORA_CHANNEL_NAME_MAX];
    size_t n = hu_molora_router_normalize_channel("telegram:42", 11, out, sizeof(out));
    HU_ASSERT_EQ(n, 8);
    HU_ASSERT(strcmp(out, "telegram") == 0);
}

static void normalizer_lowercases_mixed_case(void) {
    char out[HU_MOLORA_CHANNEL_NAME_MAX];
    size_t n = hu_molora_router_normalize_channel("Telegram", 8, out, sizeof(out));
    HU_ASSERT_EQ(n, 8);
    HU_ASSERT(strcmp(out, "telegram") == 0);
}

static void normalizer_trims_whitespace_and_lowercases(void) {
    char out[HU_MOLORA_CHANNEL_NAME_MAX];
    size_t n = hu_molora_router_normalize_channel(" Telegram \t", 11, out, sizeof(out));
    HU_ASSERT_EQ(n, 8);
    HU_ASSERT(strcmp(out, "telegram") == 0);
}

static void normalizer_empty_input_returns_zero(void) {
    char out[HU_MOLORA_CHANNEL_NAME_MAX];
    size_t n = hu_molora_router_normalize_channel("", 0, out, sizeof(out));
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT(out[0] == '\0');
}

static void normalizer_colon_only_returns_zero(void) {
    char out[HU_MOLORA_CHANNEL_NAME_MAX];
    size_t n = hu_molora_router_normalize_channel(":42", 3, out, sizeof(out));
    HU_ASSERT_EQ(n, 0);
}

static void normalizer_truncates_to_buffer(void) {
    char in[64];
    memset(in, 'a', sizeof(in));
    char out[HU_MOLORA_CHANNEL_NAME_MAX];
    size_t n = hu_molora_router_normalize_channel(in, sizeof(in), out, sizeof(out));
    HU_ASSERT_EQ(n, sizeof(out) - 1);
    HU_ASSERT(out[sizeof(out) - 1] == '\0');
}

static void normalizer_handles_imessage_with_chat_id(void) {
    /* daemon_proactive.c populates iMessage active_channel as
     * "imessage:<chat_id>" — the router must collapse to "imessage". */
    char out[HU_MOLORA_CHANNEL_NAME_MAX];
    size_t n = hu_molora_router_normalize_channel("imessage:+15551234567", 22, out, sizeof(out));
    HU_ASSERT_EQ(n, 8);
    HU_ASSERT(strcmp(out, "imessage") == 0);
}

static void normalizer_handles_null_out(void) {
    /* Defensive: NULL out buffer returns 0 without crashing. */
    size_t n = hu_molora_router_normalize_channel("telegram", 8, NULL, 16);
    HU_ASSERT_EQ(n, 0);
}

/* ── Router select behavior ───────────────────────────────────────────── */

/* AC-7.8.4: zero-init is a valid disabled state. */
static void test_router_zero_init_is_disabled(void) {
    hu_molora_router_t r = {0};
    /* select must safely return NULL on a zero-init router. */
    const char *p = hu_molora_router_select(&r, "telegram", 8);
    HU_ASSERT(p == NULL);
    /* enabled flag must be false. */
    HU_ASSERT(r.enabled == false);
    HU_ASSERT(r.count == 0);
}

/* AC-7.8.3: disabled router (enabled=false) never returns an adapter even
 * when entries are populated. This is the "kill switch" path: a user toggles
 * molora.enabled = false in config and the router becomes a no-op without
 * needing to clear the channel_adapters map. */
static void test_disabled_molora_no_call(void) {
    hu_molora_router_t r = {0};
    r.enabled = false;
    r.count = 1;
    strcpy(r.entries[0].channel, "telegram");
    r.entries[0].adapter_path = "/tmp/should_not_be_returned.lora";
    r.default_adapter_path = "/tmp/default.lora";

    /* When disabled, select returns NULL regardless of channel match. */
    HU_ASSERT(hu_molora_router_select(&r, "telegram", 8) == NULL);
    HU_ASSERT(hu_molora_router_select(&r, "unknown", 7) == NULL);
    HU_ASSERT(hu_molora_router_select(&r, "", 0) == NULL);
}

/* AC-7.8.1: enabled router with telegram→adapter selects the telegram
 * adapter when active_channel == "telegram". */
static void test_static_router_selects_channel_adapter(void) {
    hu_molora_router_t r = {0};
    r.enabled = true;
    r.count = 2;
    strcpy(r.entries[0].channel, "telegram");
    r.entries[0].adapter_path = "/tmp/telegram.lora";
    strcpy(r.entries[1].channel, "slack");
    r.entries[1].adapter_path = "/tmp/slack.lora";
    r.default_adapter_path = "/tmp/default.lora";

    const char *p = hu_molora_router_select(&r, "telegram", 8);
    HU_ASSERT(p != NULL);
    HU_ASSERT(strcmp(p, "/tmp/telegram.lora") == 0);

    /* Also verify the slack entry resolves correctly. */
    p = hu_molora_router_select(&r, "slack", 5);
    HU_ASSERT(p != NULL);
    HU_ASSERT(strcmp(p, "/tmp/slack.lora") == 0);
}

/* AC-7.8.1 + R2 risk: normalization must apply at lookup. "telegram:42"
 * resolves to the same entry as "telegram". */
static void test_static_router_normalizes_lookup(void) {
    hu_molora_router_t r = {0};
    r.enabled = true;
    r.count = 1;
    strcpy(r.entries[0].channel, "telegram");
    r.entries[0].adapter_path = "/tmp/telegram.lora";

    HU_ASSERT(strcmp(hu_molora_router_select(&r, "telegram:42", 11), "/tmp/telegram.lora") == 0);
    HU_ASSERT(strcmp(hu_molora_router_select(&r, "Telegram", 8), "/tmp/telegram.lora") == 0);
    HU_ASSERT(strcmp(hu_molora_router_select(&r, " Telegram \t", 11), "/tmp/telegram.lora") == 0);
}

/* AC-7.8.2: channel not in map falls back to default_adapter_path; no error
 * is emitted (the test framework would catch fatals). */
static void test_static_router_fallback_to_default(void) {
    hu_molora_router_t r = {0};
    r.enabled = true;
    r.count = 1;
    strcpy(r.entries[0].channel, "telegram");
    r.entries[0].adapter_path = "/tmp/telegram.lora";
    r.default_adapter_path = "/tmp/default.lora";

    /* "irc" is not in the map → fall back to default. */
    const char *p = hu_molora_router_select(&r, "irc", 3);
    HU_ASSERT(p != NULL);
    HU_ASSERT(strcmp(p, "/tmp/default.lora") == 0);
}

/* AC-7.8.2 variant: when no entry matches AND default is NULL, return NULL
 * (caller falls through to "no adapter swap"). */
static void test_static_router_no_default_returns_null(void) {
    hu_molora_router_t r = {0};
    r.enabled = true;
    r.count = 1;
    strcpy(r.entries[0].channel, "telegram");
    r.entries[0].adapter_path = "/tmp/telegram.lora";
    r.default_adapter_path = NULL;

    HU_ASSERT(hu_molora_router_select(&r, "irc", 3) == NULL);
}

/* Empty channel id must surface default (not crash, not match arbitrarily). */
static void test_static_router_empty_channel_returns_default(void) {
    hu_molora_router_t r = {0};
    r.enabled = true;
    r.count = 1;
    strcpy(r.entries[0].channel, "telegram");
    r.entries[0].adapter_path = "/tmp/telegram.lora";
    r.default_adapter_path = "/tmp/default.lora";

    HU_ASSERT(strcmp(hu_molora_router_select(&r, "", 0), "/tmp/default.lora") == 0);
    HU_ASSERT(strcmp(hu_molora_router_select(&r, NULL, 0), "/tmp/default.lora") == 0);
}

/* NULL router pointer must not crash. */
static void test_static_router_null_pointer_safe(void) {
    HU_ASSERT(hu_molora_router_select(NULL, "telegram", 8) == NULL);
}

/* ── Router init from config ──────────────────────────────────────────── */

/* hu_molora_router_init builds the router from cfg->personalization.molora.
 * NULL config → disabled router. */
static void test_router_init_null_config_disabled(void) {
    hu_molora_router_t r;
    memset(&r, 0xAA, sizeof(r)); /* poison to verify _init zeros */
    HU_ASSERT_EQ(hu_molora_router_init(&r, NULL), HU_OK);
    HU_ASSERT(r.enabled == false);
    HU_ASSERT_EQ(r.count, 0);
}

/* NULL router pointer → invalid argument. */
static void test_router_init_null_router_invalid(void) {
    HU_ASSERT_EQ(hu_molora_router_init(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* Config with molora disabled → router disabled but default path is wired
 * (so the caller's gating only checks `enabled`, not the path). */
static void test_router_init_personalization_disabled_keeps_router_off(void) {
    hu_config_t cfg = {0};
    cfg.personalization.enabled = false;
    cfg.personalization.molora.enabled = true;
    cfg.personalization.molora.count = 1;
    strcpy(cfg.personalization.molora.entries[0].channel, "telegram");
    /* Casting away const is safe here: the parser strdups; this fixture
     * does not exercise the free path. */
    cfg.personalization.molora.entries[0].adapter_path = (char *)"/tmp/t.lora";

    hu_molora_router_t r;
    HU_ASSERT_EQ(hu_molora_router_init(&r, &cfg), HU_OK);
    /* personalization.enabled = false → router stays disabled even though
     * molora.enabled = true (design Q2). */
    HU_ASSERT(r.enabled == false);
}

/* Config with both personalization.enabled and molora.enabled → router
 * is fully populated. */
static void test_router_init_populates_from_config(void) {
    hu_config_t cfg = {0};
    cfg.personalization.enabled = true;
    cfg.personalization.lora_adapter_path = (char *)"/tmp/default.lora";
    cfg.personalization.molora.enabled = true;
    cfg.personalization.molora.count = 2;
    strcpy(cfg.personalization.molora.entries[0].channel, "telegram");
    cfg.personalization.molora.entries[0].adapter_path = (char *)"/tmp/t.lora";
    strcpy(cfg.personalization.molora.entries[1].channel, "slack");
    cfg.personalization.molora.entries[1].adapter_path = (char *)"/tmp/s.lora";

    hu_molora_router_t r;
    HU_ASSERT_EQ(hu_molora_router_init(&r, &cfg), HU_OK);
    HU_ASSERT(r.enabled == true);
    HU_ASSERT_EQ(r.count, 2);
    HU_ASSERT(strcmp(r.default_adapter_path, "/tmp/default.lora") == 0);
    HU_ASSERT(strcmp(hu_molora_router_select(&r, "telegram", 8), "/tmp/t.lora") == 0);
    HU_ASSERT(strcmp(hu_molora_router_select(&r, "slack", 5), "/tmp/s.lora") == 0);
    HU_ASSERT(strcmp(hu_molora_router_select(&r, "discord", 7), "/tmp/default.lora") == 0);
}

/* Sanity: AC-7.8.5 binary budget check is a CI-side script, not a runtime
 * test, but we can at least pin the struct size so anyone bloating it has
 * to update this test deliberately. */
static void test_router_struct_size_is_bounded(void) {
    /* Soft bound: 16 entries * (32-byte name + 8-byte ptr) + overhead.
     * If this fires, someone added a field — review against AC-7.8.5
     * before bumping the limit. */
    HU_ASSERT(sizeof(hu_molora_router_t) < 2048);
}

#endif /* HU_ENABLE_MOLORA */

void run_molora_router_tests(void);
void run_molora_router_tests(void) {
    HU_TEST_SUITE("MoloraRouter");
#ifdef HU_ENABLE_MOLORA
    HU_RUN_TEST(normalizer_lowercase_pass_through);
    HU_RUN_TEST(normalizer_strips_colon_suffix);
    HU_RUN_TEST(normalizer_lowercases_mixed_case);
    HU_RUN_TEST(normalizer_trims_whitespace_and_lowercases);
    HU_RUN_TEST(normalizer_empty_input_returns_zero);
    HU_RUN_TEST(normalizer_colon_only_returns_zero);
    HU_RUN_TEST(normalizer_truncates_to_buffer);
    HU_RUN_TEST(normalizer_handles_imessage_with_chat_id);
    HU_RUN_TEST(normalizer_handles_null_out);
    HU_RUN_TEST(test_router_zero_init_is_disabled);
    HU_RUN_TEST(test_disabled_molora_no_call);
    HU_RUN_TEST(test_static_router_selects_channel_adapter);
    HU_RUN_TEST(test_static_router_normalizes_lookup);
    HU_RUN_TEST(test_static_router_fallback_to_default);
    HU_RUN_TEST(test_static_router_no_default_returns_null);
    HU_RUN_TEST(test_static_router_empty_channel_returns_default);
    HU_RUN_TEST(test_static_router_null_pointer_safe);
    HU_RUN_TEST(test_router_init_null_config_disabled);
    HU_RUN_TEST(test_router_init_null_router_invalid);
    HU_RUN_TEST(test_router_init_personalization_disabled_keeps_router_off);
    HU_RUN_TEST(test_router_init_populates_from_config);
    HU_RUN_TEST(test_router_struct_size_is_bounded);
#endif /* HU_ENABLE_MOLORA */
}
