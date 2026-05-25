/* tests/test_onboard_dispatcher.c
 *
 * Sprint 51 US-C2.1 PART 2 — dispatcher tests.
 *
 * Tests use SCRIPTED step vtables whose run() returns a sequence of preset
 * results (NEXT, BACK, REPEAT, etc.) read from user_data. That exercises
 * every state-machine transition without depending on any specific step's
 * real implementation.
 */

#include "human/core/error.h"
#include "human/onboard/dispatcher.h"
#include "human/onboard/state.h"
#include "human/onboard/step.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct scripted_step_data {
    hu_onboard_step_result_t script[16];
    size_t script_len;
    size_t script_idx;
    int run_count;
    int enter_count;
} scripted_step_data_t;

static hu_onboard_step_result_t scripted_run(hu_onboard_step_t *self, hu_onboard_state_t *state) {
    (void)state;
    scripted_step_data_t *d = (scripted_step_data_t *)self->user_data;
    d->run_count++;
    if (d->script_idx >= d->script_len) {
        return HU_ONBOARD_COMPLETE;
    }
    return d->script[d->script_idx++];
}

static void scripted_enter(hu_onboard_step_t *self, hu_onboard_state_t *state) {
    (void)state;
    scripted_step_data_t *d = (scripted_step_data_t *)self->user_data;
    d->enter_count++;
}

static void build_scripted_table(hu_onboard_step_t steps[HU_ONBOARD_STEP_COMPLETE],
                                 scripted_step_data_t data[HU_ONBOARD_STEP_COMPLETE],
                                 hu_onboard_dispatcher_config_t *cfg) {
    static const char *names[] = {"welcome", "provider", "persona", "channels", "testsend"};
    for (size_t i = 0; i < HU_ONBOARD_STEP_COMPLETE; i++) {
        memset(&data[i], 0, sizeof(data[i]));
        memset(&steps[i], 0, sizeof(steps[i]));
        steps[i].name = names[i];
        steps[i].display_name = names[i];
        steps[i].run = scripted_run;
        steps[i].enter = scripted_enter;
        steps[i].user_data = &data[i];
        cfg->step_table[i] = &steps[i];
    }
}

/* ── next_step pure helper ─────────────────────────────────────────── */

static void test_next_step_progresses_through_all_5(void) {
    HU_ASSERT_EQ((int)hu_onboard_next_step(HU_ONBOARD_STEP_WELCOME), (int)HU_ONBOARD_STEP_PROVIDER);
    HU_ASSERT_EQ((int)hu_onboard_next_step(HU_ONBOARD_STEP_PROVIDER), (int)HU_ONBOARD_STEP_PERSONA);
    HU_ASSERT_EQ((int)hu_onboard_next_step(HU_ONBOARD_STEP_PERSONA), (int)HU_ONBOARD_STEP_CHANNELS);
    HU_ASSERT_EQ((int)hu_onboard_next_step(HU_ONBOARD_STEP_CHANNELS),
                 (int)HU_ONBOARD_STEP_TESTSEND);
    HU_ASSERT_EQ((int)hu_onboard_next_step(HU_ONBOARD_STEP_TESTSEND),
                 (int)HU_ONBOARD_STEP_COMPLETE);
}

static void test_next_step_complete_is_idempotent(void) {
    HU_ASSERT_EQ((int)hu_onboard_next_step(HU_ONBOARD_STEP_COMPLETE),
                 (int)HU_ONBOARD_STEP_COMPLETE);
}

/* ── dispatcher loop ───────────────────────────────────────────────── */

static void test_dispatcher_rejects_null_state(void) {
    hu_onboard_dispatcher_config_t cfg = {0};
    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(NULL, &cfg), (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_rejects_null_config(void) {
    hu_onboard_state_t state;
    hu_onboard_state_init(&state);
    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(&state, NULL), (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_rejects_missing_vtable(void) {
    hu_onboard_state_t state;
    hu_onboard_state_init(&state);
    hu_onboard_dispatcher_config_t cfg = {0};
    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(&state, &cfg), (int)HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_walks_all_5_steps_to_complete(void) {
    hu_onboard_step_t steps[HU_ONBOARD_STEP_COMPLETE];
    scripted_step_data_t data[HU_ONBOARD_STEP_COMPLETE];
    hu_onboard_dispatcher_config_t cfg = {0};
    build_scripted_table(steps, data, &cfg);

    for (size_t i = 0; i < HU_ONBOARD_STEP_COMPLETE; i++) {
        data[i].script[0] = HU_ONBOARD_NEXT;
        data[i].script_len = 1;
    }

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    hu_error_t err = hu_onboard_dispatcher_run(&state, &cfg);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)state.current, (int)HU_ONBOARD_STEP_COMPLETE);

    for (size_t i = 0; i < HU_ONBOARD_STEP_COMPLETE; i++) {
        HU_ASSERT_EQ(data[i].run_count, 1);
        HU_ASSERT_EQ(data[i].enter_count, 1);
    }
}

static void test_dispatcher_quit_mid_flow_returns_ok_and_preserves_current(void) {
    hu_onboard_step_t steps[HU_ONBOARD_STEP_COMPLETE];
    scripted_step_data_t data[HU_ONBOARD_STEP_COMPLETE];
    hu_onboard_dispatcher_config_t cfg = {0};
    build_scripted_table(steps, data, &cfg);

    data[HU_ONBOARD_STEP_WELCOME].script[0] = HU_ONBOARD_NEXT;
    data[HU_ONBOARD_STEP_WELCOME].script_len = 1;
    data[HU_ONBOARD_STEP_PROVIDER].script[0] = HU_ONBOARD_QUIT;
    data[HU_ONBOARD_STEP_PROVIDER].script_len = 1;

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(&state, &cfg), (int)HU_OK);
    HU_ASSERT_EQ((int)state.current, (int)HU_ONBOARD_STEP_PROVIDER);
    HU_ASSERT_EQ(data[HU_ONBOARD_STEP_PERSONA].run_count, 0);
    HU_ASSERT_EQ(data[HU_ONBOARD_STEP_CHANNELS].run_count, 0);
}

static void test_dispatcher_repeat_keeps_current_step(void) {
    hu_onboard_step_t steps[HU_ONBOARD_STEP_COMPLETE];
    scripted_step_data_t data[HU_ONBOARD_STEP_COMPLETE];
    hu_onboard_dispatcher_config_t cfg = {0};
    build_scripted_table(steps, data, &cfg);

    data[HU_ONBOARD_STEP_WELCOME].script[0] = HU_ONBOARD_NEXT;
    data[HU_ONBOARD_STEP_WELCOME].script_len = 1;
    data[HU_ONBOARD_STEP_PROVIDER].script[0] = HU_ONBOARD_REPEAT;
    data[HU_ONBOARD_STEP_PROVIDER].script[1] = HU_ONBOARD_QUIT;
    data[HU_ONBOARD_STEP_PROVIDER].script_len = 2;

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(&state, &cfg), (int)HU_OK);
    HU_ASSERT_EQ(data[HU_ONBOARD_STEP_PROVIDER].run_count, 2);
    HU_ASSERT_EQ((int)state.current, (int)HU_ONBOARD_STEP_PROVIDER);
}

static void test_dispatcher_back_from_provider_returns_to_welcome(void) {
    hu_onboard_step_t steps[HU_ONBOARD_STEP_COMPLETE];
    scripted_step_data_t data[HU_ONBOARD_STEP_COMPLETE];
    hu_onboard_dispatcher_config_t cfg = {0};
    build_scripted_table(steps, data, &cfg);

    data[HU_ONBOARD_STEP_WELCOME].script[0] = HU_ONBOARD_NEXT;
    data[HU_ONBOARD_STEP_WELCOME].script[1] = HU_ONBOARD_QUIT;
    data[HU_ONBOARD_STEP_WELCOME].script_len = 2;
    data[HU_ONBOARD_STEP_PROVIDER].script[0] = HU_ONBOARD_BACK;
    data[HU_ONBOARD_STEP_PROVIDER].script_len = 1;

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(&state, &cfg), (int)HU_OK);
    HU_ASSERT_EQ(data[HU_ONBOARD_STEP_WELCOME].run_count, 2);
    HU_ASSERT_EQ(data[HU_ONBOARD_STEP_PROVIDER].run_count, 1);
    HU_ASSERT_EQ((int)state.current, (int)HU_ONBOARD_STEP_WELCOME);
}

static void test_dispatcher_back_from_step_0_with_empty_history_is_noop(void) {
    hu_onboard_step_t steps[HU_ONBOARD_STEP_COMPLETE];
    scripted_step_data_t data[HU_ONBOARD_STEP_COMPLETE];
    hu_onboard_dispatcher_config_t cfg = {0};
    build_scripted_table(steps, data, &cfg);

    data[HU_ONBOARD_STEP_WELCOME].script[0] = HU_ONBOARD_BACK;
    data[HU_ONBOARD_STEP_WELCOME].script[1] = HU_ONBOARD_QUIT;
    data[HU_ONBOARD_STEP_WELCOME].script_len = 2;

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(&state, &cfg), (int)HU_OK);
    HU_ASSERT_EQ(data[HU_ONBOARD_STEP_WELCOME].run_count, 2);
    HU_ASSERT_EQ((int)state.current, (int)HU_ONBOARD_STEP_WELCOME);
}

static void test_dispatcher_abort_returns_internal_error(void) {
    hu_onboard_step_t steps[HU_ONBOARD_STEP_COMPLETE];
    scripted_step_data_t data[HU_ONBOARD_STEP_COMPLETE];
    hu_onboard_dispatcher_config_t cfg = {0};
    build_scripted_table(steps, data, &cfg);

    data[HU_ONBOARD_STEP_WELCOME].script[0] = HU_ONBOARD_ABORT;
    data[HU_ONBOARD_STEP_WELCOME].script_len = 1;

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(&state, &cfg), (int)HU_ERR_INTERNAL);
    HU_ASSERT_EQ((int)state.current, (int)HU_ONBOARD_STEP_WELCOME);
}

static void test_dispatcher_complete_result_terminates_loop(void) {
    hu_onboard_step_t steps[HU_ONBOARD_STEP_COMPLETE];
    scripted_step_data_t data[HU_ONBOARD_STEP_COMPLETE];
    hu_onboard_dispatcher_config_t cfg = {0};
    build_scripted_table(steps, data, &cfg);

    data[HU_ONBOARD_STEP_WELCOME].script[0] = HU_ONBOARD_COMPLETE;
    data[HU_ONBOARD_STEP_WELCOME].script_len = 1;

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(&state, &cfg), (int)HU_OK);
    HU_ASSERT_EQ((int)state.current, (int)HU_ONBOARD_STEP_COMPLETE);
    HU_ASSERT_EQ(data[HU_ONBOARD_STEP_PROVIDER].run_count, 0);
}

static void test_dispatcher_persists_state_to_path_on_every_transition(void) {
    hu_onboard_step_t steps[HU_ONBOARD_STEP_COMPLETE];
    scripted_step_data_t data[HU_ONBOARD_STEP_COMPLETE];
    hu_onboard_dispatcher_config_t cfg = {0};
    build_scripted_table(steps, data, &cfg);

    data[HU_ONBOARD_STEP_WELCOME].script[0] = HU_ONBOARD_NEXT;
    data[HU_ONBOARD_STEP_WELCOME].script_len = 1;
    data[HU_ONBOARD_STEP_PROVIDER].script[0] = HU_ONBOARD_QUIT;
    data[HU_ONBOARD_STEP_PROVIDER].script_len = 1;

    char tmp_path[] = "/tmp/hu_dispatcher_save_XXXXXX";
    int fd = mkstemp(tmp_path);
    HU_ASSERT_TRUE(fd >= 0);
    close(fd);
    unlink(tmp_path);

    hu_onboard_state_t state;
    hu_onboard_state_init(&state);
    cfg.state_path = tmp_path;

    HU_ASSERT_EQ((int)hu_onboard_dispatcher_run(&state, &cfg), (int)HU_OK);

    hu_onboard_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    HU_ASSERT_EQ((int)hu_onboard_state_load(&loaded, tmp_path), (int)HU_OK);
    HU_ASSERT_EQ((int)loaded.current, (int)HU_ONBOARD_STEP_PROVIDER);

    unlink(tmp_path);
}

void run_onboard_dispatcher_tests(void) {
    HU_TEST_SUITE("onboard_dispatcher");
    HU_RUN_TEST(test_next_step_progresses_through_all_5);
    HU_RUN_TEST(test_next_step_complete_is_idempotent);
    HU_RUN_TEST(test_dispatcher_rejects_null_state);
    HU_RUN_TEST(test_dispatcher_rejects_null_config);
    HU_RUN_TEST(test_dispatcher_rejects_missing_vtable);
    HU_RUN_TEST(test_dispatcher_walks_all_5_steps_to_complete);
    HU_RUN_TEST(test_dispatcher_quit_mid_flow_returns_ok_and_preserves_current);
    HU_RUN_TEST(test_dispatcher_repeat_keeps_current_step);
    HU_RUN_TEST(test_dispatcher_back_from_provider_returns_to_welcome);
    HU_RUN_TEST(test_dispatcher_back_from_step_0_with_empty_history_is_noop);
    HU_RUN_TEST(test_dispatcher_abort_returns_internal_error);
    HU_RUN_TEST(test_dispatcher_complete_result_terminates_loop);
    HU_RUN_TEST(test_dispatcher_persists_state_to_path_on_every_transition);
}
