/* Compile-time check that all Phase 2 vtable headers parse correctly.
 * Runtime tests verify concrete implementations have required vtable fields. */
#include "human/channels/cli.h"
#include "human/memory.h"
#include "human/observability/log_observer.h"
#include "human/observer.h"
#include "human/peripheral.h"
#include "human/providers/factory.h"
#include "human/runtime.h"
#include "human/human.h"
#include "human/tools/file_read.h"
#include "test_framework.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Ensure vtables are complete and usable. */
static void test_provider_vtable_size(void) {
    (void)sizeof(hu_provider_vtable_t);
    (void)sizeof(hu_provider_t);
}

static void test_channel_vtable_size(void) {
    (void)sizeof(hu_channel_vtable_t);
    (void)sizeof(hu_channel_t);
}

static void test_tool_vtable_size(void) {
    (void)sizeof(hu_tool_vtable_t);
    (void)sizeof(hu_tool_t);
}

static void test_memory_vtable_size(void) {
    (void)sizeof(hu_memory_vtable_t);
    (void)sizeof(hu_memory_t);
    (void)sizeof(hu_session_store_vtable_t);
    (void)sizeof(hu_session_store_t);
}

static void test_observer_vtable_size(void) {
    (void)sizeof(hu_observer_vtable_t);
    (void)sizeof(hu_observer_t);
}

static void test_runtime_vtable_size(void) {
    (void)sizeof(hu_runtime_vtable_t);
    (void)sizeof(hu_runtime_t);
}

static void test_peripheral_vtable_size(void) {
    (void)sizeof(hu_peripheral_vtable_t);
    (void)sizeof(hu_peripheral_t);
}

static void test_tool_result_constructors(void) {
    hu_tool_result_t ok = hu_tool_result_ok("output", 6);
    HU_ASSERT_TRUE(ok.success);
    HU_ASSERT_STR_EQ(ok.output, "output");
    HU_ASSERT_EQ(ok.output_len, 6u);
    hu_tool_result_t fail = hu_tool_result_fail("error", 5);
    HU_ASSERT_FALSE(fail.success);
    HU_ASSERT_STR_EQ(fail.error_msg, "error");
    HU_ASSERT_EQ(fail.error_msg_len, 5u);
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
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_provider_create(&alloc, "openai", 6, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prov.ctx);
    HU_ASSERT_NOT_NULL(prov.vtable);

    HU_ASSERT_NOT_NULL(prov.vtable->chat);
    HU_ASSERT_NOT_NULL(prov.vtable->get_name);
    HU_ASSERT_NOT_NULL(prov.vtable->deinit);
    HU_ASSERT_NOT_NULL(prov.vtable->supports_native_tools);

    const char *name = prov.vtable->get_name(prov.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);

    prov.vtable->deinit(prov.ctx, &alloc);
}

/* ─── Channel: cli ───────────────────────────────────────────────────────── */
static void test_channel_cli_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_error_t err = hu_cli_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_NOT_NULL(ch.vtable);

    HU_ASSERT_NOT_NULL(ch.vtable->start);
    HU_ASSERT_NOT_NULL(ch.vtable->stop);
    HU_ASSERT_NOT_NULL(ch.vtable->send);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    HU_ASSERT_NOT_NULL(ch.vtable->health_check);

    const char *name = ch.vtable->name(ch.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);

    hu_cli_destroy(&ch);
}

/* ─── Tool: file_read ───────────────────────────────────────────────────── */
static void test_tool_file_read_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_file_read_create(&alloc, ".", 1, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.ctx);
    HU_ASSERT_NOT_NULL(tool.vtable);

    HU_ASSERT_NOT_NULL(tool.vtable->execute);
    HU_ASSERT_NOT_NULL(tool.vtable->name);
    HU_ASSERT_NOT_NULL(tool.vtable->description);
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json);

    const char *name = tool.vtable->name(tool.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);

    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Memory: none ──────────────────────────────────────────────────────── */
static void test_memory_none_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_NOT_NULL(mem.vtable);

    HU_ASSERT_NOT_NULL(mem.vtable->name);
    HU_ASSERT_NOT_NULL(mem.vtable->store);
    HU_ASSERT_NOT_NULL(mem.vtable->recall);
    HU_ASSERT_NOT_NULL(mem.vtable->get);
    HU_ASSERT_NOT_NULL(mem.vtable->list);
    HU_ASSERT_NOT_NULL(mem.vtable->forget);
    HU_ASSERT_NOT_NULL(mem.vtable->count);
    HU_ASSERT_NOT_NULL(mem.vtable->health_check);
    HU_ASSERT_NOT_NULL(mem.vtable->deinit);

    const char *name = mem.vtable->name(mem.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);

    mem.vtable->deinit(mem.ctx);
}

#ifdef HU_ENABLE_SQLITE
/* ─── Memory: sqlite ────────────────────────────────────────────────────── */
static void test_memory_sqlite_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_NOT_NULL(mem.vtable);

    HU_ASSERT_NOT_NULL(mem.vtable->name);
    HU_ASSERT_NOT_NULL(mem.vtable->store);
    HU_ASSERT_NOT_NULL(mem.vtable->recall);
    const char *name = mem.vtable->name(mem.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);

    mem.vtable->deinit(mem.ctx);
}
#endif

/* ─── Observer: log ─────────────────────────────────────────────────────── */
static void test_observer_log_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&alloc, f);
    HU_ASSERT_NOT_NULL(obs.ctx);
    HU_ASSERT_NOT_NULL(obs.vtable);

    HU_ASSERT_NOT_NULL(obs.vtable->record_event);
    HU_ASSERT_NOT_NULL(obs.vtable->record_metric);
    HU_ASSERT_NOT_NULL(obs.vtable->flush);
    HU_ASSERT_NOT_NULL(obs.vtable->name);
    HU_ASSERT_NOT_NULL(obs.vtable->deinit);

    const char *name = obs.vtable->name(obs.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);

    if (obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
    fclose(f);
}

/* ─── Runtime: native ───────────────────────────────────────────────────── */
static void test_runtime_native_vtable(void) {
    hu_runtime_t r = hu_runtime_native();
    HU_ASSERT_NOT_NULL(r.ctx);
    HU_ASSERT_NOT_NULL(r.vtable);

    HU_ASSERT_NOT_NULL(r.vtable->name);
    HU_ASSERT_NOT_NULL(r.vtable->has_shell_access);
    HU_ASSERT_NOT_NULL(r.vtable->has_filesystem_access);
    HU_ASSERT_NOT_NULL(r.vtable->storage_path);
    HU_ASSERT_NOT_NULL(r.vtable->supports_long_running);
    HU_ASSERT_NOT_NULL(r.vtable->memory_budget);

    const char *name = r.vtable->name(r.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);
}

/* ─── Peripheral: arduino ────────────────────────────────────────────────── */
static void test_peripheral_arduino_vtable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_config_t config = {
        .serial_port = "/dev/cu.usbmodem0",
        .serial_port_len = 17,
        .chip = NULL,
        .chip_len = 0,
    };
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(&alloc, "arduino", 7, &config, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_NOT_NULL(p.vtable);

    HU_ASSERT_NOT_NULL(p.vtable->name);
    HU_ASSERT_NOT_NULL(p.vtable->board_type);
    HU_ASSERT_NOT_NULL(p.vtable->health_check);
    HU_ASSERT_NOT_NULL(p.vtable->destroy);

    const char *name = p.vtable->name(p.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_TRUE(strlen(name) > 0);

    p.vtable->destroy(p.ctx, &alloc);
}

void test_vtables_run(void) {
    HU_TEST_SUITE("vtables");
    HU_RUN_TEST(test_vtable_headers_compile);
    HU_RUN_TEST(test_provider_openai_vtable);
    HU_RUN_TEST(test_channel_cli_vtable);
    HU_RUN_TEST(test_tool_file_read_vtable);
    HU_RUN_TEST(test_memory_none_vtable);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_memory_sqlite_vtable);
#endif
    HU_RUN_TEST(test_observer_log_vtable);
    HU_RUN_TEST(test_runtime_native_vtable);
    HU_RUN_TEST(test_peripheral_arduino_vtable);
}
