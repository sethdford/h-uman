/* tests/test_doctor_install.c — US-43.4 `human doctor --install` predicate.
 *
 * Tests `hu_doctor_check_install` with stack-allocated state structs. No
 * filesystem I/O (story-mandated test seam). Pins:
 *   AC-43.4.1: all-green → HU_OK + 4 HU_DIAG_OK items
 *   AC-43.4.2: malformed persona → HU_DIAG_ERR containing "persona" AND
 *              "parse"; overall return still HU_OK
 *   AC-43.4.4: remediation hints "human onboard", "human channel pair imessage"
 *   Always-4 contract: every red path still emits exactly 4 items (no
 *   short-circuit).
 *
 * Per ~/.claude/rules/tests-that-pin-bugs.md: adversarial assertions check
 * the dangerous case is BLOCKED — red paths assert HU_ASSERT_NE(severity,
 * HU_DIAG_OK), not the weaker "rc was HU_OK". */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_tests_run = 0;
static int s_tests_passed = 0;

#define RUN(fn)           \
    do {                  \
        s_tests_run++;    \
        fn();             \
        s_tests_passed++; \
    } while (0)

static void *test_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void test_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static hu_allocator_t s_alloc = {.alloc = test_alloc, .free = test_free};

/* Free buffer + item strings; shared cleanup for every test. */
static void free_items(hu_diag_item_t *items, size_t count, size_t cap) {
    for (size_t i = 0; i < count; i++) {
        if (items[i].category)
            free((void *)items[i].category);
        if (items[i].message)
            free((void *)items[i].message);
    }
    free(items);
    (void)cap;
}

static hu_diag_item_t *fresh_buffer(size_t *cap_out) {
    *cap_out = 8;
    hu_diag_item_t *buf = (hu_diag_item_t *)malloc(sizeof(hu_diag_item_t) * (*cap_out));
    assert(buf != NULL);
    return buf;
}

/* AC-43.4.1: all-green state returns HU_OK and emits exactly 4 HU_DIAG_OK items. */
static void test_doctor_install_all_green_returns_ok_with_four_ok_items(void) {
    hu_doctor_install_state_t state = {
        .binary_path = "/usr/local/bin/human",
        .config_dir_exists = true,
        .channel_paired = true,
        .persona_status = HU_DOCTOR_PERSONA_PRESENT_VALID,
    };
    size_t cap = 0;
    hu_diag_item_t *items = fresh_buffer(&cap);
    size_t count = 0;

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &state, &items, &count, &cap);
    assert(rc == HU_OK);
    assert(count == 4);
    for (size_t i = 0; i < count; i++) {
        assert(items[i].severity == HU_DIAG_OK);
        assert(items[i].message != NULL);
    }
    free_items(items, count, cap);
}

/* Binary path NULL is item[0] ERR; remaining items still emit OK; total == 4. */
static void test_doctor_install_missing_binary_path_reports_err(void) {
    hu_doctor_install_state_t state = {
        .binary_path = NULL,
        .config_dir_exists = true,
        .channel_paired = true,
        .persona_status = HU_DOCTOR_PERSONA_PRESENT_VALID,
    };
    size_t cap = 0;
    hu_diag_item_t *items = fresh_buffer(&cap);
    size_t count = 0;

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &state, &items, &count, &cap);
    assert(rc == HU_OK);
    assert(count == 4); /* no short-circuit */
    assert(items[0].severity == HU_DIAG_ERR);
    assert(items[1].severity == HU_DIAG_OK);
    assert(items[2].severity == HU_DIAG_OK);
    assert(items[3].severity == HU_DIAG_OK);
    free_items(items, count, cap);
}

/* AC-43.4.4: config_dir false → item[1] ERR with "human onboard" hint. */
static void test_doctor_install_missing_config_dir_reports_err_with_human_onboard_hint(void) {
    hu_doctor_install_state_t state = {
        .binary_path = "/usr/local/bin/human",
        .config_dir_exists = false,
        .channel_paired = true,
        .persona_status = HU_DOCTOR_PERSONA_PRESENT_VALID,
    };
    size_t cap = 0;
    hu_diag_item_t *items = fresh_buffer(&cap);
    size_t count = 0;

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &state, &items, &count, &cap);
    assert(rc == HU_OK);
    assert(count == 4);
    assert(items[1].severity == HU_DIAG_ERR);
    assert(items[1].message != NULL);
    assert(strstr(items[1].message, "human onboard") != NULL);
    free_items(items, count, cap);
}

/* AC-43.4.4: channel_paired false → item[2] ERR with "human channel pair imessage". */
static void test_doctor_install_no_channel_paired_reports_err_with_pair_imessage_hint(void) {
    hu_doctor_install_state_t state = {
        .binary_path = "/usr/local/bin/human",
        .config_dir_exists = true,
        .channel_paired = false,
        .persona_status = HU_DOCTOR_PERSONA_PRESENT_VALID,
    };
    size_t cap = 0;
    hu_diag_item_t *items = fresh_buffer(&cap);
    size_t count = 0;

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &state, &items, &count, &cap);
    assert(rc == HU_OK);
    assert(count == 4);
    assert(items[2].severity == HU_DIAG_ERR);
    assert(items[2].message != NULL);
    assert(strstr(items[2].message, "human channel pair imessage") != NULL);
    free_items(items, count, cap);
}

/* Persona MISSING → item[3] ERR containing "persona"; predicate still HU_OK. */
static void test_doctor_install_persona_missing_reports_err_containing_persona(void) {
    hu_doctor_install_state_t state = {
        .binary_path = "/usr/local/bin/human",
        .config_dir_exists = true,
        .channel_paired = true,
        .persona_status = HU_DOCTOR_PERSONA_MISSING,
    };
    size_t cap = 0;
    hu_diag_item_t *items = fresh_buffer(&cap);
    size_t count = 0;

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &state, &items, &count, &cap);
    assert(rc == HU_OK);
    assert(count == 4);
    assert(items[3].severity == HU_DIAG_ERR);
    assert(items[3].message != NULL);
    assert(strstr(items[3].message, "persona") != NULL);
    free_items(items, count, cap);
}

/* AC-43.4.2: malformed persona → ERR containing both "persona" AND "parse",
 * predicate return still HU_OK. */
static void test_doctor_install_persona_invalid_reports_err_containing_persona_and_parse(void) {
    hu_doctor_install_state_t state = {
        .binary_path = "/usr/local/bin/human",
        .config_dir_exists = true,
        .channel_paired = true,
        .persona_status = HU_DOCTOR_PERSONA_PRESENT_INVALID,
    };
    size_t cap = 0;
    hu_diag_item_t *items = fresh_buffer(&cap);
    size_t count = 0;

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &state, &items, &count, &cap);
    assert(rc == HU_OK); /* AC-43.4.2 mandates HU_OK on malformed persona */
    assert(count == 4);
    assert(items[3].severity == HU_DIAG_ERR);
    assert(items[3].message != NULL);
    assert(strstr(items[3].message, "persona") != NULL);
    assert(strstr(items[3].message, "parse") != NULL);
    free_items(items, count, cap);
}

/* All-red state still emits exactly 4 items — no short-circuit on failure. */
static void test_doctor_install_all_red_still_emits_exactly_four_items(void) {
    hu_doctor_install_state_t state = {
        .binary_path = NULL,
        .config_dir_exists = false,
        .channel_paired = false,
        .persona_status = HU_DOCTOR_PERSONA_MISSING,
    };
    size_t cap = 0;
    hu_diag_item_t *items = fresh_buffer(&cap);
    size_t count = 0;

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &state, &items, &count, &cap);
    assert(rc == HU_OK);
    assert(count == 4); /* all-red is still the 4-item contract */
    for (size_t i = 0; i < count; i++) {
        assert(items[i].severity == HU_DIAG_ERR);
        assert(items[i].message != NULL);
    }
    /* Spot-check remediation hints still present on the inverted side. */
    assert(strstr(items[1].message, "human onboard") != NULL);
    assert(strstr(items[2].message, "human channel pair imessage") != NULL);
    free_items(items, count, cap);
}

int run_doctor_install_tests(void) {
    s_tests_run = 0;
    s_tests_passed = 0;

    RUN(test_doctor_install_all_green_returns_ok_with_four_ok_items);
    RUN(test_doctor_install_missing_binary_path_reports_err);
    RUN(test_doctor_install_missing_config_dir_reports_err_with_human_onboard_hint);
    RUN(test_doctor_install_no_channel_paired_reports_err_with_pair_imessage_hint);
    RUN(test_doctor_install_persona_missing_reports_err_containing_persona);
    RUN(test_doctor_install_persona_invalid_reports_err_containing_persona_and_parse);
    RUN(test_doctor_install_all_red_still_emits_exactly_four_items);

    printf("  doctor_install: %d/%d passed\n", s_tests_passed, s_tests_run);
    return s_tests_run - s_tests_passed;
}
