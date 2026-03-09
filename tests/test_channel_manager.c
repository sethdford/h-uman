/* Channel manager tests */
#include "seaclaw/bus.h"
#include "seaclaw/channel_manager.h"
#include "seaclaw/channels/cli.h"
#include "seaclaw/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void test_channel_manager_init_deinit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_manager_t mgr;
    sc_error_t err = sc_channel_manager_init(&mgr, &alloc);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sc_channel_manager_count(&mgr), 0);
    sc_channel_manager_deinit(&mgr);
}

static void test_channel_manager_register(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t cli;
    sc_error_t err = sc_cli_create(&alloc, &cli);
    SC_ASSERT_EQ(err, SC_OK);

    sc_channel_manager_t mgr;
    sc_channel_manager_init(&mgr, &alloc);
    err = sc_channel_manager_register(&mgr, "cli", NULL, &cli, SC_CHANNEL_LISTENER_POLLING);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sc_channel_manager_count(&mgr), 1);

    size_t count = 0;
    const sc_channel_entry_t *entries = sc_channel_manager_entries(&mgr, &count);
    SC_ASSERT_EQ(count, 1);
    SC_ASSERT_TRUE(strcmp(entries[0].name, "cli") == 0);

    sc_channel_manager_deinit(&mgr);
    sc_cli_destroy(&cli);
}

static void test_channel_manager_set_bus(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_manager_t mgr;
    sc_channel_manager_init(&mgr, &alloc);
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_channel_manager_set_bus(&mgr, &bus);
    /* No assert - just ensure it doesn't crash */
    sc_channel_manager_deinit(&mgr);
}

static void test_channel_manager_start_stop(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t cli;
    sc_cli_create(&alloc, &cli);
    sc_channel_manager_t mgr;
    sc_channel_manager_init(&mgr, &alloc);
    sc_channel_manager_register(&mgr, "cli", NULL, &cli, SC_CHANNEL_LISTENER_POLLING);

    sc_error_t err = sc_channel_manager_start_all(&mgr);
    SC_ASSERT_TRUE(err == SC_OK || err == SC_ERR_CHANNEL_START);
    sc_channel_manager_stop_all(&mgr);

    sc_channel_manager_deinit(&mgr);
    sc_cli_destroy(&cli);
}

static void test_channel_manager_count_starts_at_zero(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_manager_t mgr;
    sc_channel_manager_init(&mgr, &alloc);
    SC_ASSERT_EQ(sc_channel_manager_count(&mgr), 0);
    sc_channel_manager_deinit(&mgr);
}

static void test_channel_manager_entries_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_manager_t mgr;
    sc_channel_manager_init(&mgr, &alloc);
    size_t count = 99;
    const sc_channel_entry_t *entries = sc_channel_manager_entries(&mgr, &count);
    SC_ASSERT_EQ(count, 0);
    SC_ASSERT_NOT_NULL(entries);
    sc_channel_manager_deinit(&mgr);
}

static void test_channel_manager_register_multiple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t cli1, cli2, cli3;
    sc_cli_create(&alloc, &cli1);
    sc_cli_create(&alloc, &cli2);
    sc_cli_create(&alloc, &cli3);

    sc_channel_manager_t mgr;
    sc_channel_manager_init(&mgr, &alloc);
    sc_channel_manager_register(&mgr, "a", NULL, &cli1, SC_CHANNEL_LISTENER_POLLING);
    sc_channel_manager_register(&mgr, "b", NULL, &cli2, SC_CHANNEL_LISTENER_GATEWAY);
    sc_channel_manager_register(&mgr, "c", NULL, &cli3, SC_CHANNEL_LISTENER_SEND_ONLY);
    SC_ASSERT_EQ(sc_channel_manager_count(&mgr), 3);

    size_t count = 0;
    const sc_channel_entry_t *entries = sc_channel_manager_entries(&mgr, &count);
    SC_ASSERT_EQ(count, 3);
    SC_ASSERT_TRUE(strcmp(entries[0].name, "a") == 0);
    SC_ASSERT_TRUE(strcmp(entries[1].name, "b") == 0);
    SC_ASSERT_TRUE(strcmp(entries[2].name, "c") == 0);

    sc_channel_manager_deinit(&mgr);
    sc_cli_destroy(&cli1);
    sc_cli_destroy(&cli2);
    sc_cli_destroy(&cli3);
}

static void test_channel_manager_stop_all_without_start(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t cli;
    sc_cli_create(&alloc, &cli);
    sc_channel_manager_t mgr;
    sc_channel_manager_init(&mgr, &alloc);
    sc_channel_manager_register(&mgr, "cli", NULL, &cli, SC_CHANNEL_LISTENER_POLLING);
    sc_channel_manager_stop_all(&mgr);
    sc_channel_manager_deinit(&mgr);
    sc_cli_destroy(&cli);
}

static void test_channel_manager_listener_types(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t cli;
    sc_cli_create(&alloc, &cli);
    sc_channel_manager_t mgr;
    sc_channel_manager_init(&mgr, &alloc);

    sc_channel_manager_register(&mgr, "poll", NULL, &cli, SC_CHANNEL_LISTENER_POLLING);
    size_t count = 0;
    const sc_channel_entry_t *entries = sc_channel_manager_entries(&mgr, &count);
    SC_ASSERT_EQ(count, 1);
    SC_ASSERT_EQ(entries[0].listener_type, SC_CHANNEL_LISTENER_POLLING);

    sc_channel_manager_deinit(&mgr);
    sc_cli_destroy(&cli);
}

static void test_channel_manager_deinit_twice_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_manager_t mgr;
    sc_channel_manager_init(&mgr, &alloc);
    sc_channel_manager_deinit(&mgr);
    sc_channel_manager_deinit(&mgr);
}

void run_channel_manager_tests(void) {
    SC_TEST_SUITE("Channel manager");
    SC_RUN_TEST(test_channel_manager_init_deinit);
    SC_RUN_TEST(test_channel_manager_register);
    SC_RUN_TEST(test_channel_manager_set_bus);
    SC_RUN_TEST(test_channel_manager_start_stop);
    SC_RUN_TEST(test_channel_manager_count_starts_at_zero);
    SC_RUN_TEST(test_channel_manager_entries_empty);
    SC_RUN_TEST(test_channel_manager_register_multiple);
    SC_RUN_TEST(test_channel_manager_stop_all_without_start);
    SC_RUN_TEST(test_channel_manager_listener_types);
    SC_RUN_TEST(test_channel_manager_deinit_twice_safe);
}
