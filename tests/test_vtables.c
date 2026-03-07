/* Compile-time check that all Phase 2 vtable headers parse correctly.
 * Runtime tests verify concrete implementations have required vtable fields. */
#include "seaclaw/channels/cli.h"
#include "seaclaw/memory.h"
#include "seaclaw/observability/log_observer.h"
#include "seaclaw/observer.h"
#include "seaclaw/peripheral.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/runtime.h"
#include "seaclaw/seaclaw.h"
#include "seaclaw/tools/file_read.h"
#include "test_framework.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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
    SC_ASSERT_TRUE(ok.success);
    SC_ASSERT_STR_EQ(ok.output, "output");
    SC_ASSERT_EQ(ok.output_len, 6u);
    sc_tool_result_t fail = sc_tool_result_fail("error", 5);
    SC_ASSERT_FALSE(fail.success);
    SC_ASSERT_STR_EQ(fail.error_msg, "error");
    SC_ASSERT_EQ(fail.error_msg_len, 5u);
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

/* ─── Provider: openai ───────────────────────────────────────────────────── */
static void test_provider_openai_vtable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov;
    sc_error_t err = sc_provider_create(&alloc, "openai", 6, "test-key", 8, NULL, 0, &prov);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prov.ctx);
    SC_ASSERT_NOT_NULL(prov.vtable);

    SC_ASSERT_NOT_NULL(prov.vtable->chat);
    SC_ASSERT_NOT_NULL(prov.vtable->get_name);
    SC_ASSERT_NOT_NULL(prov.vtable->deinit);
    SC_ASSERT_NOT_NULL(prov.vtable->supports_native_tools);

    const char *name = prov.vtable->get_name(prov.ctx);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);

    prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Channel: cli ───────────────────────────────────────────────────────── */
static void test_channel_cli_vtable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_t ch;
    sc_error_t err = sc_cli_create(&alloc, &ch);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(ch.ctx);
    SC_ASSERT_NOT_NULL(ch.vtable);

    SC_ASSERT_NOT_NULL(ch.vtable->start);
    SC_ASSERT_NOT_NULL(ch.vtable->stop);
    SC_ASSERT_NOT_NULL(ch.vtable->send);
    SC_ASSERT_NOT_NULL(ch.vtable->name);
    SC_ASSERT_NOT_NULL(ch.vtable->health_check);

    const char *name = ch.vtable->name(ch.ctx);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);

    sc_cli_destroy(&ch);
}

/* ─── Tool: file_read ───────────────────────────────────────────────────── */
static void test_tool_file_read_vtable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_file_read_create(&alloc, ".", 1, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(tool.ctx);
    SC_ASSERT_NOT_NULL(tool.vtable);

    SC_ASSERT_NOT_NULL(tool.vtable->execute);
    SC_ASSERT_NOT_NULL(tool.vtable->name);
    SC_ASSERT_NOT_NULL(tool.vtable->description);
    SC_ASSERT_NOT_NULL(tool.vtable->parameters_json);

    const char *name = tool.vtable->name(tool.ctx);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);

    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Memory: none ──────────────────────────────────────────────────────── */
static void test_memory_none_vtable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    SC_ASSERT_NOT_NULL(mem.ctx);
    SC_ASSERT_NOT_NULL(mem.vtable);

    SC_ASSERT_NOT_NULL(mem.vtable->name);
    SC_ASSERT_NOT_NULL(mem.vtable->store);
    SC_ASSERT_NOT_NULL(mem.vtable->recall);
    SC_ASSERT_NOT_NULL(mem.vtable->get);
    SC_ASSERT_NOT_NULL(mem.vtable->list);
    SC_ASSERT_NOT_NULL(mem.vtable->forget);
    SC_ASSERT_NOT_NULL(mem.vtable->count);
    SC_ASSERT_NOT_NULL(mem.vtable->health_check);
    SC_ASSERT_NOT_NULL(mem.vtable->deinit);

    const char *name = mem.vtable->name(mem.ctx);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);

    mem.vtable->deinit(mem.ctx);
}

#ifdef SC_ENABLE_SQLITE
/* ─── Memory: sqlite ────────────────────────────────────────────────────── */
static void test_memory_sqlite_vtable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.ctx);
    SC_ASSERT_NOT_NULL(mem.vtable);

    SC_ASSERT_NOT_NULL(mem.vtable->name);
    SC_ASSERT_NOT_NULL(mem.vtable->store);
    SC_ASSERT_NOT_NULL(mem.vtable->recall);
    const char *name = mem.vtable->name(mem.ctx);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);

    mem.vtable->deinit(mem.ctx);
}
#endif

/* ─── Observer: log ─────────────────────────────────────────────────────── */
static void test_observer_log_vtable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    FILE *f = tmpfile();
    SC_ASSERT_NOT_NULL(f);
    sc_observer_t obs = sc_log_observer_create(&alloc, f);
    SC_ASSERT_NOT_NULL(obs.ctx);
    SC_ASSERT_NOT_NULL(obs.vtable);

    SC_ASSERT_NOT_NULL(obs.vtable->record_event);
    SC_ASSERT_NOT_NULL(obs.vtable->record_metric);
    SC_ASSERT_NOT_NULL(obs.vtable->flush);
    SC_ASSERT_NOT_NULL(obs.vtable->name);
    SC_ASSERT_NOT_NULL(obs.vtable->deinit);

    const char *name = obs.vtable->name(obs.ctx);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);

    if (obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
    fclose(f);
}

/* ─── Runtime: native ───────────────────────────────────────────────────── */
static void test_runtime_native_vtable(void) {
    sc_runtime_t r = sc_runtime_native();
    SC_ASSERT_NOT_NULL(r.ctx);
    SC_ASSERT_NOT_NULL(r.vtable);

    SC_ASSERT_NOT_NULL(r.vtable->name);
    SC_ASSERT_NOT_NULL(r.vtable->has_shell_access);
    SC_ASSERT_NOT_NULL(r.vtable->has_filesystem_access);
    SC_ASSERT_NOT_NULL(r.vtable->storage_path);
    SC_ASSERT_NOT_NULL(r.vtable->supports_long_running);
    SC_ASSERT_NOT_NULL(r.vtable->memory_budget);

    const char *name = r.vtable->name(r.ctx);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);
}

/* ─── Peripheral: arduino ────────────────────────────────────────────────── */
static void test_peripheral_arduino_vtable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_config_t config = {
        .serial_port = "/dev/cu.usbmodem0",
        .serial_port_len = 17,
        .chip = NULL,
        .chip_len = 0,
    };
    sc_peripheral_t p;
    sc_error_t err = sc_peripheral_create(&alloc, "arduino", 7, &config, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(p.ctx);
    SC_ASSERT_NOT_NULL(p.vtable);

    SC_ASSERT_NOT_NULL(p.vtable->name);
    SC_ASSERT_NOT_NULL(p.vtable->board_type);
    SC_ASSERT_NOT_NULL(p.vtable->health_check);
    SC_ASSERT_NOT_NULL(p.vtable->destroy);

    const char *name = p.vtable->name(p.ctx);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_TRUE(strlen(name) > 0);

    p.vtable->destroy(p.ctx, &alloc);
}

void test_vtables_run(void) {
    SC_TEST_SUITE("vtables");
    SC_RUN_TEST(test_vtable_headers_compile);
    SC_RUN_TEST(test_provider_openai_vtable);
    SC_RUN_TEST(test_channel_cli_vtable);
    SC_RUN_TEST(test_tool_file_read_vtable);
    SC_RUN_TEST(test_memory_none_vtable);
#ifdef SC_ENABLE_SQLITE
    SC_RUN_TEST(test_memory_sqlite_vtable);
#endif
    SC_RUN_TEST(test_observer_log_vtable);
    SC_RUN_TEST(test_runtime_native_vtable);
    SC_RUN_TEST(test_peripheral_arduino_vtable);
}
