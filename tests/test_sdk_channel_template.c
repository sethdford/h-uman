/*
 * Public channel-SDK contract test (Task 12 of
 * docs/plans/2026-09-20-dead-code-plan.md).
 *
 * src/channels/channel_manager.c has no caller inside the daemon — the agent
 * drives channels through channel_loop.c — so it now lives in human_devlib
 * rather than human_core. What keeps it alive is that it is DOCUMENTED PUBLIC
 * API: docs/api/channels.md, sdk/README.md, and the copy-paste starting point
 * at sdk/templates/channel/. This file is the executable form of that claim.
 *
 * It does three things no other test does:
 *   1. compiles sdk/templates/channel/my_channel.h against
 *      include/human/channel_manager.h in one translation unit, so a change to
 *      either header that breaks the pair fails the build;
 *   2. runs the exact sequence the template's own doc comment prescribes
 *      (hu_my_channel_create -> hu_channel_manager_register), so the snippet
 *      an SDK user copies is known to work;
 *   3. pins hu_channel_manager_register's signature by taking its address
 *      through a spelled-out function-pointer type — a silent reorder or
 *      retype of the parameters stops compiling here.
 *
 * sdk/templates/channel/my_channel.c is compiled into human_tests (see the
 * add_executable(human_tests ...) source list) so the registration below is
 * over a real vtable, not a stub.
 */
#include "human/channel.h"
#include "human/channel_manager.h"
#include "human/core/allocator.h"
#include "my_channel.h" /* sdk/templates/channel/my_channel.h */
#include "test_framework.h"
#include <string.h>

/* (3) The register() signature the SDK docs publish. If channel_manager.h
 * changes any parameter type or order, this initializer fails to compile. */
typedef hu_error_t (*hu_sdk_register_fn)(hu_channel_manager_t *, const char *, const char *,
                                         const hu_channel_t *, hu_channel_listener_type_t);

static void test_sdk_channel_template_register_signature(void) {
    hu_sdk_register_fn reg = hu_channel_manager_register;
    HU_ASSERT_TRUE(reg != NULL);
}

/* (2) The sequence from the template header's doc comment, verbatim. */
static void test_sdk_channel_template_registers(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    HU_ASSERT_EQ(hu_my_channel_create(&alloc, &ch), HU_OK);
    HU_ASSERT_TRUE(ch.vtable != NULL);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "my_channel");

    hu_channel_manager_t mgr;
    HU_ASSERT_EQ(hu_channel_manager_init(&mgr, &alloc), HU_OK);
    /* Pre-condition: the registry is empty, so the count assertion below
     * can only pass because the register call did something. */
    HU_ASSERT_EQ(hu_channel_manager_count(&mgr), 0);

    HU_ASSERT_EQ(hu_channel_manager_register(&mgr, "my_channel", "default", &ch,
                                             HU_CHANNEL_LISTENER_SEND_ONLY),
                 HU_OK);
    HU_ASSERT_EQ(hu_channel_manager_count(&mgr), 1);

    size_t count = 0;
    const hu_channel_entry_t *entries = hu_channel_manager_entries(&mgr, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_TRUE(entries != NULL);
    HU_ASSERT_STR_EQ(entries[0].name, "my_channel");
    HU_ASSERT_EQ(entries[0].listener_type, HU_CHANNEL_LISTENER_SEND_ONLY);

    hu_channel_manager_deinit(&mgr);
    hu_my_channel_destroy(&ch);
    HU_ASSERT_TRUE(ch.vtable == NULL);
}

/* The template's vtable declares start/stop/send/name/health_check and
 * leaves every optional method NULL — the contract documented at the top of
 * sdk/templates/channel/my_channel.c and in include/human/channel.h. */
static void test_sdk_channel_template_vtable_contract(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    HU_ASSERT_EQ(hu_my_channel_create(&alloc, &ch), HU_OK);

    HU_ASSERT_TRUE(ch.vtable->start != NULL);
    HU_ASSERT_TRUE(ch.vtable->stop != NULL);
    HU_ASSERT_TRUE(ch.vtable->send != NULL);
    HU_ASSERT_TRUE(ch.vtable->name != NULL);
    HU_ASSERT_TRUE(ch.vtable->health_check != NULL);

    /* health_check is false before start, true after — proves the ctx the
     * template allocates is actually threaded through the vtable. */
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx) == false);
    HU_ASSERT_EQ(ch.vtable->start(ch.ctx), HU_OK);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx) == true);
    ch.vtable->stop(ch.ctx);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx) == false);

    hu_my_channel_destroy(&ch);
}

void run_sdk_channel_template_tests(void) {
    HU_TEST_SUITE("SDK channel template");
    HU_RUN_TEST(test_sdk_channel_template_register_signature);
    HU_RUN_TEST(test_sdk_channel_template_registers);
    HU_RUN_TEST(test_sdk_channel_template_vtable_contract);
}
