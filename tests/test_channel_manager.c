/* Channel manager tests */
#include "test_framework.h"
#include "seaclaw/channel_manager.h"
#include "seaclaw/channels/cli.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/bus.h"
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

void run_channel_manager_tests(void) {
    SC_TEST_SUITE("Channel manager");
    SC_RUN_TEST(test_channel_manager_init_deinit);
    SC_RUN_TEST(test_channel_manager_register);
    SC_RUN_TEST(test_channel_manager_set_bus);
    SC_RUN_TEST(test_channel_manager_start_stop);
}
