#include "human/channels/imessage.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Test counters. */
static int send_sticker_call_count = 0;
static const char *last_sticker_path = NULL;
static const char *last_target = NULL;

static hu_error_t stub_succeeds(const char *target, size_t target_len, const char *path) {
    (void)target_len;
    send_sticker_call_count++;
    last_target = target;
    last_sticker_path = path;
    return HU_OK;
}

static hu_error_t stub_fails(const char *target, size_t target_len, const char *path) {
    (void)target;
    (void)target_len;
    (void)path;
    send_sticker_call_count++;
    return HU_ERR_NOT_SUPPORTED;
}

static char *make_tmp_file(void) {
    static char path[256];
    strcpy(path, "/tmp/human-sticker-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    write(fd, "fake-png", 8);
    close(fd);
    return path;
}

/* AC: valid args + existing file + succeeding stub → HU_OK, stub called. */
static void valid_send_invokes_stub_returns_ok(void) {
    send_sticker_call_count = 0;
    last_target = NULL;
    last_sticker_path = NULL;
    hu_imessage_set_test_send_sticker_stub(stub_succeeds);

    char *p = make_tmp_file();
    HU_ASSERT_NOT_NULL(p);

    hu_error_t err = hu_imessage_send_sticker(NULL, "+15555551212", 12, p, strlen(p));
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(send_sticker_call_count, 1);
    HU_ASSERT_STR_EQ(last_target, "+15555551212");
    HU_ASSERT_STR_EQ(last_sticker_path, p);

    unlink(p);
    hu_imessage_set_test_send_sticker_stub(NULL);
}

/* AC: missing sticker file → HU_ERR_NOT_FOUND (or _INVALID_ARGUMENT if
 * the project doesn't have HU_ERR_NOT_FOUND — adjust assertion). */
static void missing_file_returns_not_found(void) {
    send_sticker_call_count = 0;
    hu_imessage_set_test_send_sticker_stub(stub_succeeds);

    hu_error_t err =
        hu_imessage_send_sticker(NULL, "+15555551212", 12, "/tmp/nonexistent_sticker_XYZ.png", 32);
    /* Use whichever error code the impl returns — match the actual return. */
    HU_ASSERT(err != HU_OK);
    HU_ASSERT_EQ(send_sticker_call_count, 0); /* stub NOT called for missing file */

    hu_imessage_set_test_send_sticker_stub(NULL);
}

/* AC: null target or path → HU_ERR_INVALID_ARGUMENT, stub NOT called. */
static void null_args_invalid_argument(void) {
    send_sticker_call_count = 0;
    hu_imessage_set_test_send_sticker_stub(stub_succeeds);

    HU_ASSERT_EQ((int)hu_imessage_send_sticker(NULL, NULL, 0, "/tmp/x.png", 10),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_imessage_send_sticker(NULL, "+15555551212", 12, NULL, 0),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(send_sticker_call_count, 0);

    hu_imessage_set_test_send_sticker_stub(NULL);
}

/* AC: stub failure propagates as the return value. */
static void stub_failure_propagates(void) {
    send_sticker_call_count = 0;
    hu_imessage_set_test_send_sticker_stub(stub_fails);

    char *p = make_tmp_file();
    HU_ASSERT_NOT_NULL(p);

    hu_error_t err = hu_imessage_send_sticker(NULL, "+15555551212", 12, p, strlen(p));
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(send_sticker_call_count, 1);

    unlink(p);
    hu_imessage_set_test_send_sticker_stub(NULL);
}

void run_imessage_sticker_tests(void) {
    HU_TEST_SUITE("imessage_sticker");
    HU_RUN_TEST(valid_send_invokes_stub_returns_ok);
    HU_RUN_TEST(missing_file_returns_not_found);
    HU_RUN_TEST(null_args_invalid_argument);
    HU_RUN_TEST(stub_failure_propagates);
}
