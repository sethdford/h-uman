// @covers-none — exercises hu_imessage_private_build_* and
// hu_imessage_private_should_route from src/channels/imessage_private/client.c.
// Filename→module heuristic resolves "imessage" (wrong, nested module), so opt
// out; production symbols are called directly.
#include "human/channels/imessage_private/client.h"
#include "test_framework.h"
#include <string.h>

/* The built command must be exactly what apps/imessage-helper's
 * IMHelper.handleMessage parses — so we assert on the literal JSON. */

static void build_send_flat_has_no_parent_fields(void) {
    char out[512];
    HU_ASSERT_EQ((int)hu_imessage_private_build_send(out, sizeof(out), "tx1", "iMessage;-;+15551234",
                                                     "hey", NULL, 0),
                 (int)HU_OK);
    HU_ASSERT_STR_EQ(out,
                     "{\"action\":\"send-message\",\"data\":{\"chatGuid\":\"iMessage;-;+15551234\","
                     "\"message\":\"hey\"},\"transactionId\":\"tx1\"}");
}

static void build_send_reply_includes_selected_guid(void) {
    char out[512];
    HU_ASSERT_EQ((int)hu_imessage_private_build_send(out, sizeof(out), "tx2", "chatG", "yes",
                                                     "PARENT-GUID", 0),
                 (int)HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "\"selectedMessageGuid\":\"PARENT-GUID\""));
    HU_ASSERT_NOT_NULL(strstr(out, "\"partIndex\":0"));
    HU_ASSERT_NOT_NULL(strstr(out, "\"action\":\"send-message\""));
}

static void build_send_escapes_quotes_and_newlines(void) {
    char out[512];
    HU_ASSERT_EQ((int)hu_imessage_private_build_send(out, sizeof(out), "t", "g",
                                                     "she said \"hi\"\nbye", NULL, 0),
                 (int)HU_OK);
    /* embedded quote must be escaped, newline must be \n (not a raw newline) */
    HU_ASSERT_NOT_NULL(strstr(out, "she said \\\"hi\\\"\\nbye"));
    HU_ASSERT_TRUE(strchr(out, '\n') == NULL);
}

static void build_send_rejects_empty_chat(void) {
    char out[64];
    HU_ASSERT_EQ((int)hu_imessage_private_build_send(out, sizeof(out), "t", "", "x", NULL, 0),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

static void build_send_too_small_buffer_errors(void) {
    char out[8];
    HU_ASSERT_EQ((int)hu_imessage_private_build_send(out, sizeof(out), "t", "g", "a long message",
                                                     NULL, 0),
                 (int)HU_ERR_LIMIT_REACHED);
}

static void build_reaction_shape(void) {
    char out[512];
    HU_ASSERT_EQ((int)hu_imessage_private_build_reaction(out, sizeof(out), "tx", "g", "P", "love", 1),
                 (int)HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "\"action\":\"send-reaction\""));
    HU_ASSERT_NOT_NULL(strstr(out, "\"reactionType\":\"love\""));
    HU_ASSERT_NOT_NULL(strstr(out, "\"selectedMessageGuid\":\"P\""));
    HU_ASSERT_NOT_NULL(strstr(out, "\"partIndex\":1"));
}

static void build_edit_shape(void) {
    char out[512];
    HU_ASSERT_EQ((int)hu_imessage_private_build_edit(out, sizeof(out), "tx", "g", "MGUID", "new",
                                                     "old", 0),
                 (int)HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "\"action\":\"edit-message\""));
    HU_ASSERT_NOT_NULL(strstr(out, "\"messageGuid\":\"MGUID\""));
    HU_ASSERT_NOT_NULL(strstr(out, "\"editedMessage\":\"new\""));
    HU_ASSERT_NOT_NULL(strstr(out, "\"backwardsCompatibilityMessage\":\"old\""));
}

static void build_unsend_shape(void) {
    char out[256];
    HU_ASSERT_EQ((int)hu_imessage_private_build_unsend(out, sizeof(out), "tx", "g", "MGUID", 2),
                 (int)HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "\"action\":\"unsend-message\""));
    HU_ASSERT_NOT_NULL(strstr(out, "\"messageGuid\":\"MGUID\""));
    HU_ASSERT_NOT_NULL(strstr(out, "\"partIndex\":2"));
}

/* ── backend-selection predicate ──────────────────────────────────────── */

static void should_route_only_when_live_enabled_connected(void) {
    HU_ASSERT_TRUE(hu_imessage_private_should_route(true, HU_IMESSAGE_PRIVATE_MODE_LIVE, true));
}

static void should_route_false_when_shadow(void) {
    /* SHADOW must NOT change the sent output → never routes to IMCore. */
    HU_ASSERT_FALSE(hu_imessage_private_should_route(true, HU_IMESSAGE_PRIVATE_MODE_SHADOW, true));
}

static void should_route_false_when_off_or_disabled_or_disconnected(void) {
    HU_ASSERT_FALSE(hu_imessage_private_should_route(true, HU_IMESSAGE_PRIVATE_MODE_OFF, true));
    HU_ASSERT_FALSE(hu_imessage_private_should_route(false, HU_IMESSAGE_PRIVATE_MODE_LIVE, true));
    HU_ASSERT_FALSE(hu_imessage_private_should_route(true, HU_IMESSAGE_PRIVATE_MODE_LIVE, false));
}

void run_imessage_private_client_tests(void) {
    HU_TEST_SUITE("imessage_private_client");
    HU_RUN_TEST(build_send_flat_has_no_parent_fields);
    HU_RUN_TEST(build_send_reply_includes_selected_guid);
    HU_RUN_TEST(build_send_escapes_quotes_and_newlines);
    HU_RUN_TEST(build_send_rejects_empty_chat);
    HU_RUN_TEST(build_send_too_small_buffer_errors);
    HU_RUN_TEST(build_reaction_shape);
    HU_RUN_TEST(build_edit_shape);
    HU_RUN_TEST(build_unsend_shape);
    HU_RUN_TEST(should_route_only_when_live_enabled_connected);
    HU_RUN_TEST(should_route_false_when_shadow);
    HU_RUN_TEST(should_route_false_when_off_or_disabled_or_disconnected);
}
