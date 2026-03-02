/* Compile-time check that all Phase 2 vtable headers parse correctly. */
#include "seaclaw/seaclaw.h"
#include "test_framework.h"
#include <stddef.h>

/* Ensure vtables are complete and usable. */
static void test_provider_vtable_size(void) {
    (void)sizeof(sc_provider_vtable_t);
    (void)sizeof(sc_provider_t);
}

static void test_channel_vtable_size(void) {
    (void)sizeof(sc_channel_vtable_t);
    (void)sizeof(sc_channel_t);
}

static void test_tool_vtable_size(void) {
    (void)sizeof(sc_tool_vtable_t);
    (void)sizeof(sc_tool_t);
}

static void test_memory_vtable_size(void) {
    (void)sizeof(sc_memory_vtable_t);
    (void)sizeof(sc_memory_t);
    (void)sizeof(sc_session_store_vtable_t);
    (void)sizeof(sc_session_store_t);
}

static void test_observer_vtable_size(void) {
    (void)sizeof(sc_observer_vtable_t);
    (void)sizeof(sc_observer_t);
}

static void test_runtime_vtable_size(void) {
    (void)sizeof(sc_runtime_vtable_t);
    (void)sizeof(sc_runtime_t);
}

static void test_peripheral_vtable_size(void) {
    (void)sizeof(sc_peripheral_vtable_t);
    (void)sizeof(sc_peripheral_t);
}

static void test_tool_result_constructors(void) {
    sc_tool_result_t ok = sc_tool_result_ok("output", 6);
    (void)ok;
    sc_tool_result_t fail = sc_tool_result_fail("error", 5);
    (void)fail;
}

static void test_vtable_headers_compile(void) {
    test_provider_vtable_size();
    test_channel_vtable_size();
    test_tool_vtable_size();
    test_memory_vtable_size();
    test_observer_vtable_size();
    test_runtime_vtable_size();
    test_peripheral_vtable_size();
    test_tool_result_constructors();
}

void test_vtables_run(void) {
    SC_TEST_SUITE("vtables");
    SC_RUN_TEST(test_vtable_headers_compile);
}
