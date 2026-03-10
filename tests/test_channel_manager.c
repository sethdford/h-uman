/* Channel manager tests */
#include "human/bus.h"
#include "human/channel_manager.h"
#include "human/channels/cli.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void test_channel_manager_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_manager_t mgr;
    hu_error_t err = hu_channel_manager_init(&mgr, &alloc);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_channel_manager_count(&mgr), 0);
    hu_channel_manager_deinit(&mgr);
}

static void test_channel_manager_register(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t cli;
    hu_error_t err = hu_cli_create(&alloc, &cli);
    HU_ASSERT_EQ(err, HU_OK);

    hu_channel_manager_t mgr;
    hu_channel_manager_init(&mgr, &alloc);
    err = hu_channel_manager_register(&mgr, "cli", NULL, &cli, HU_CHANNEL_LISTENER_POLLING);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_channel_manager_count(&mgr), 1);

    size_t count = 0;
    const hu_channel_entry_t *entries = hu_channel_manager_entries(&mgr, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_TRUE(strcmp(entries[0].name, "cli") == 0);

    hu_channel_manager_deinit(&mgr);
    hu_cli_destroy(&cli);
}

static void test_channel_manager_set_bus(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_manager_t mgr;
    hu_channel_manager_init(&mgr, &alloc);
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_channel_manager_set_bus(&mgr, &bus);
    /* No assert - just ensure it doesn't crash */
    hu_channel_manager_deinit(&mgr);
}

static void test_channel_manager_start_stop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t cli;
    hu_cli_create(&alloc, &cli);
    hu_channel_manager_t mgr;
    hu_channel_manager_init(&mgr, &alloc);
    hu_channel_manager_register(&mgr, "cli", NULL, &cli, HU_CHANNEL_LISTENER_POLLING);

    hu_error_t err = hu_channel_manager_start_all(&mgr);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_CHANNEL_START);
    hu_channel_manager_stop_all(&mgr);

    hu_channel_manager_deinit(&mgr);
    hu_cli_destroy(&cli);
}

static void test_channel_manager_count_starts_at_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_manager_t mgr;
    hu_channel_manager_init(&mgr, &alloc);
    HU_ASSERT_EQ(hu_channel_manager_count(&mgr), 0);
    hu_channel_manager_deinit(&mgr);
}

static void test_channel_manager_entries_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_manager_t mgr;
    hu_channel_manager_init(&mgr, &alloc);
    size_t count = 99;
    const hu_channel_entry_t *entries = hu_channel_manager_entries(&mgr, &count);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_NOT_NULL(entries);
    hu_channel_manager_deinit(&mgr);
}

static void test_channel_manager_register_multiple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t cli1, cli2, cli3;
    hu_cli_create(&alloc, &cli1);
    hu_cli_create(&alloc, &cli2);
    hu_cli_create(&alloc, &cli3);

    hu_channel_manager_t mgr;
    hu_channel_manager_init(&mgr, &alloc);
    hu_channel_manager_register(&mgr, "a", NULL, &cli1, HU_CHANNEL_LISTENER_POLLING);
    hu_channel_manager_register(&mgr, "b", NULL, &cli2, HU_CHANNEL_LISTENER_GATEWAY);
    hu_channel_manager_register(&mgr, "c", NULL, &cli3, HU_CHANNEL_LISTENER_SEND_ONLY);
    HU_ASSERT_EQ(hu_channel_manager_count(&mgr), 3);

    size_t count = 0;
    const hu_channel_entry_t *entries = hu_channel_manager_entries(&mgr, &count);
    HU_ASSERT_EQ(count, 3);
    HU_ASSERT_TRUE(strcmp(entries[0].name, "a") == 0);
    HU_ASSERT_TRUE(strcmp(entries[1].name, "b") == 0);
    HU_ASSERT_TRUE(strcmp(entries[2].name, "c") == 0);

    hu_channel_manager_deinit(&mgr);
    hu_cli_destroy(&cli1);
    hu_cli_destroy(&cli2);
    hu_cli_destroy(&cli3);
}

static void test_channel_manager_stop_all_without_start(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t cli;
    hu_cli_create(&alloc, &cli);
    hu_channel_manager_t mgr;
    hu_channel_manager_init(&mgr, &alloc);
    hu_channel_manager_register(&mgr, "cli", NULL, &cli, HU_CHANNEL_LISTENER_POLLING);
    hu_channel_manager_stop_all(&mgr);
    hu_channel_manager_deinit(&mgr);
    hu_cli_destroy(&cli);
}

static void test_channel_manager_listener_types(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t cli;
    hu_cli_create(&alloc, &cli);
    hu_channel_manager_t mgr;
    hu_channel_manager_init(&mgr, &alloc);

    hu_channel_manager_register(&mgr, "poll", NULL, &cli, HU_CHANNEL_LISTENER_POLLING);
    size_t count = 0;
    const hu_channel_entry_t *entries = hu_channel_manager_entries(&mgr, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_EQ(entries[0].listener_type, HU_CHANNEL_LISTENER_POLLING);

    hu_channel_manager_deinit(&mgr);
    hu_cli_destroy(&cli);
}

static void test_channel_manager_deinit_twice_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_manager_t mgr;
    hu_channel_manager_init(&mgr, &alloc);
    hu_channel_manager_deinit(&mgr);
    hu_channel_manager_deinit(&mgr);
}

void run_channel_manager_tests(void) {
    HU_TEST_SUITE("Channel manager");
    HU_RUN_TEST(test_channel_manager_init_deinit);
    HU_RUN_TEST(test_channel_manager_register);
    HU_RUN_TEST(test_channel_manager_set_bus);
    HU_RUN_TEST(test_channel_manager_start_stop);
    HU_RUN_TEST(test_channel_manager_count_starts_at_zero);
    HU_RUN_TEST(test_channel_manager_entries_empty);
    HU_RUN_TEST(test_channel_manager_register_multiple);
    HU_RUN_TEST(test_channel_manager_stop_all_without_start);
    HU_RUN_TEST(test_channel_manager_listener_types);
    HU_RUN_TEST(test_channel_manager_deinit_twice_safe);
}
