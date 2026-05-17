#include "test_framework.h"
#include "human/agent/activation_steering.h"

static void test_steering_set_init_null_returns_error(void)
{
    hu_error_t err = hu_steering_set_init(NULL, hu_system_allocator());
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_steering_set_init_succeeds(void)
{
    hu_steering_set_t set;
    hu_error_t err = hu_steering_set_init(&set, hu_system_allocator());
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(set.directives);
    HU_ASSERT_EQ(set.count, 0);
    HU_ASSERT_EQ(set.capacity, HU_STEERING_SET_INITIAL_CAP);
    hu_steering_set_deinit(&set);
}

static void test_steering_set_add_basic(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_error_t err = hu_steering_set_add(&set, "warmth", 0.5f);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(set.count, 1);
    HU_ASSERT_STR_EQ(set.directives[0].direction, "warmth");
    HU_ASSERT_FLOAT_EQ(set.directives[0].weight, 0.5f, 0.001f);
    HU_ASSERT_TRUE(set.directives[0].active);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_add_null_direction_returns_error(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    HU_ASSERT_EQ(hu_steering_set_add(&set, NULL, 0.5f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_steering_set_add(&set, "", 0.5f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(set.count, 0);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_add_weight_out_of_range(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    HU_ASSERT_EQ(hu_steering_set_add(&set, "warmth", 1.5f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_steering_set_add(&set, "warmth", -1.5f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(set.count, 0);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_add_boundary_weights(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    HU_ASSERT_EQ(hu_steering_set_add(&set, "warmth", 1.0f), HU_OK);
    HU_ASSERT_EQ(hu_steering_set_add(&set, "formality", -1.0f), HU_OK);
    HU_ASSERT_EQ(hu_steering_set_add(&set, "humor", 0.0f), HU_OK);
    HU_ASSERT_EQ(set.count, 3);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_add_duplicate_updates_weight(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_steering_set_add(&set, "warmth", 0.3f);
    HU_ASSERT_EQ(set.count, 1);

    hu_steering_set_add(&set, "warmth", 0.8f);
    HU_ASSERT_EQ(set.count, 1);
    HU_ASSERT_FLOAT_EQ(set.directives[0].weight, 0.8f, 0.001f);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_add_grows_capacity(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    char name[32];
    for (int i = 0; i < (int)HU_STEERING_SET_INITIAL_CAP + 2; i++) {
        snprintf(name, sizeof(name), "dir_%d", i);
        HU_ASSERT_EQ(hu_steering_set_add(&set, name, 0.1f * (float)i), HU_OK);
    }
    HU_ASSERT_EQ(set.count, HU_STEERING_SET_INITIAL_CAP + 2);
    HU_ASSERT_GT(set.capacity, HU_STEERING_SET_INITIAL_CAP);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_remove_existing(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_steering_set_add(&set, "warmth", 0.5f);
    hu_steering_set_add(&set, "formality", -0.3f);
    HU_ASSERT_EQ(set.count, 2);

    HU_ASSERT_EQ(hu_steering_set_remove(&set, "warmth"), HU_OK);
    HU_ASSERT_EQ(set.count, 1);
    HU_ASSERT_STR_EQ(set.directives[0].direction, "formality");

    hu_steering_set_deinit(&set);
}

static void test_steering_set_remove_not_found(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_steering_set_add(&set, "warmth", 0.5f);
    HU_ASSERT_EQ(hu_steering_set_remove(&set, "nonexistent"), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(set.count, 1);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_remove_null_returns_error(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    HU_ASSERT_EQ(hu_steering_set_remove(&set, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_steering_set_remove(NULL, "warmth"), HU_ERR_INVALID_ARGUMENT);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_render_empty_returns_null(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_steering_set_render(&set, &alloc, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_render_positive_weight(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_steering_set_add(&set, "warmth", 0.75f);

    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_steering_set_render(&set, &alloc, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "lean toward");
    HU_ASSERT_STR_CONTAINS(out, "warmth");
    HU_ASSERT_STR_CONTAINS(out, "0.75");

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_steering_set_deinit(&set);
}

static void test_steering_set_render_negative_weight(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_steering_set_add(&set, "sarcasm", -0.60f);

    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_steering_set_render(&set, &alloc, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "avoid");
    HU_ASSERT_STR_CONTAINS(out, "sarcasm");
    HU_ASSERT_STR_CONTAINS(out, "0.60");

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_steering_set_deinit(&set);
}

static void test_steering_set_render_mixed_weights(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_steering_set_add(&set, "warmth", 0.50f);
    hu_steering_set_add(&set, "verbosity", -0.40f);

    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_steering_set_render(&set, &alloc, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "lean toward: warmth");
    HU_ASSERT_STR_CONTAINS(out, "avoid: verbosity");

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_steering_set_deinit(&set);
}

static void test_steering_set_render_skips_inactive(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_steering_set_add(&set, "warmth", 0.50f);
    hu_steering_set_add(&set, "formality", 0.30f);
    set.directives[1].active = false;

    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_steering_set_render(&set, &alloc, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "warmth");
    HU_ASSERT_STR_NOT_CONTAINS(out, "formality");

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_steering_set_deinit(&set);
}

static void test_steering_set_render_all_inactive_returns_null(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_steering_set_add(&set, "warmth", 0.50f);
    set.directives[0].active = false;

    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_steering_set_render(&set, &alloc, &out, &out_len), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_render_null_args_return_error(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_steering_set_render(NULL, &alloc, &out, &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_steering_set_render(&set, NULL, &out, &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_steering_set_render(&set, &alloc, NULL, &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_steering_set_render(&set, &alloc, &out, NULL),
                 HU_ERR_INVALID_ARGUMENT);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_render_zero_weight(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    hu_steering_set_add(&set, "neutral_axis", 0.0f);

    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_steering_set_render(&set, &alloc, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_CONTAINS(out, "lean toward: neutral_axis");
    HU_ASSERT_STR_CONTAINS(out, "0.00");

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_steering_set_deinit(&set);
}

static void test_steering_set_deinit_null_is_safe(void)
{
    hu_steering_set_deinit(NULL);
}

static void test_steering_set_direction_too_long(void)
{
    hu_steering_set_t set;
    hu_steering_set_init(&set, hu_system_allocator());

    char long_dir[HU_STEERING_DIRECTION_MAX_LEN + 10];
    memset(long_dir, 'a', sizeof(long_dir) - 1);
    long_dir[sizeof(long_dir) - 1] = '\0';

    HU_ASSERT_EQ(hu_steering_set_add(&set, long_dir, 0.5f),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(set.count, 0);

    hu_steering_set_deinit(&set);
}

static void test_steering_set_tracking_allocator_no_leaks(void)
{
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    hu_steering_set_t set;
    hu_steering_set_init(&set, alloc);

    hu_steering_set_add(&set, "warmth", 0.5f);
    hu_steering_set_add(&set, "formality", -0.3f);
    hu_steering_set_add(&set, "humor", 0.9f);

    char *out = NULL;
    size_t out_len = 0;
    hu_steering_set_render(&set, &alloc, &out, &out_len);
    if (out)
        alloc.free(alloc.ctx, out, out_len + 1);

    hu_steering_set_deinit(&set);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

void run_activation_steering_tests(void)
{
    HU_TEST_SUITE("activation_steering");
    HU_RUN_TEST(test_steering_set_init_null_returns_error);
    HU_RUN_TEST(test_steering_set_init_succeeds);
    HU_RUN_TEST(test_steering_set_add_basic);
    HU_RUN_TEST(test_steering_set_add_null_direction_returns_error);
    HU_RUN_TEST(test_steering_set_add_weight_out_of_range);
    HU_RUN_TEST(test_steering_set_add_boundary_weights);
    HU_RUN_TEST(test_steering_set_add_duplicate_updates_weight);
    HU_RUN_TEST(test_steering_set_add_grows_capacity);
    HU_RUN_TEST(test_steering_set_remove_existing);
    HU_RUN_TEST(test_steering_set_remove_not_found);
    HU_RUN_TEST(test_steering_set_remove_null_returns_error);
    HU_RUN_TEST(test_steering_set_render_empty_returns_null);
    HU_RUN_TEST(test_steering_set_render_positive_weight);
    HU_RUN_TEST(test_steering_set_render_negative_weight);
    HU_RUN_TEST(test_steering_set_render_mixed_weights);
    HU_RUN_TEST(test_steering_set_render_skips_inactive);
    HU_RUN_TEST(test_steering_set_render_all_inactive_returns_null);
    HU_RUN_TEST(test_steering_set_render_null_args_return_error);
    HU_RUN_TEST(test_steering_set_render_zero_weight);
    HU_RUN_TEST(test_steering_set_deinit_null_is_safe);
    HU_RUN_TEST(test_steering_set_direction_too_long);
    HU_RUN_TEST(test_steering_set_tracking_allocator_no_leaks);
}
