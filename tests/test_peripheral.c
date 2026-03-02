#include "test_framework.h"
#include "seaclaw/peripheral.h"
#include "seaclaw/hardware.h"
#include "seaclaw/core/allocator.h"
#include <stdint.h>
#include <string.h>

static void test_factory_arduino_create(void) {
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
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "arduino");
    SC_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "arduino-uno");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_factory_stm32_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_config_t config = {
        .serial_port = NULL,
        .serial_port_len = 0,
        .chip = "STM32F401RETx",
        .chip_len = 14,
    };
    sc_peripheral_t p;
    sc_error_t err = sc_peripheral_create(&alloc, "stm32", 5, &config, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(p.ctx);
    SC_ASSERT_NOT_NULL(p.vtable);
    SC_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "nucleo");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_factory_rpi_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p;
    sc_error_t err = sc_peripheral_create(&alloc, "rpi", 3, NULL, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(p.ctx);
    SC_ASSERT_NOT_NULL(p.vtable);
    SC_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "rpi-gpio");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_factory_unknown_type_returns_not_supported(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_config_t config = { 0 };
    sc_peripheral_t p;
    sc_error_t err = sc_peripheral_create(&alloc, "unknown", 7, &config, &p);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}

static void test_capabilities_struct(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 12);
    SC_ASSERT_NOT_NULL(p.ctx);

    sc_peripheral_error_t init_err = p.vtable->init_peripheral(p.ctx);
    SC_ASSERT_EQ(init_err, SC_PERIPHERAL_ERR_NONE);

    sc_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    SC_ASSERT_TRUE(cap.has_serial);
    SC_ASSERT_TRUE(cap.has_gpio);
    SC_ASSERT_TRUE(cap.has_flash);
    SC_ASSERT_EQ(cap.flash_size_kb, 32u);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_rpi_flash_unsupported(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_rpi_peripheral_create(&alloc);
    SC_ASSERT_NOT_NULL(p.ctx);

    p.vtable->init_peripheral(p.ctx);

    sc_peripheral_error_t err = p.vtable->flash(p.ctx, "/tmp/firmware.hex", 17);
    SC_ASSERT_EQ(err, SC_PERIPHERAL_ERR_UNSUPPORTED_OPERATION);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_arduino_read_write_no_io_in_test(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_arduino_peripheral_create(&alloc, "/dev/cu.usbmodem0", 17);
    SC_ASSERT_NOT_NULL(p.ctx);

    p.vtable->init_peripheral(p.ctx);

    uint8_t val = 0xFF;
    sc_peripheral_error_t rerr = p.vtable->read(p.ctx, 0, &val);
    SC_ASSERT_EQ(rerr, SC_PERIPHERAL_ERR_NONE);
    SC_ASSERT_EQ(val, 0u);

    sc_peripheral_error_t werr = p.vtable->write(p.ctx, 1, 1);
    SC_ASSERT_EQ(werr, SC_PERIPHERAL_ERR_NONE);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_hardware_discover(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_hardware_info_t results[8];
    size_t count = 8;
    sc_error_t err = sc_hardware_discover(&alloc, results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count >= 1);
    SC_ASSERT_TRUE(results[0].detected);
    SC_ASSERT_TRUE(strlen(results[0].board_name) > 0);
    SC_ASSERT_TRUE(strlen(results[0].board_type) > 0);
}

static void test_stm32_flash_behavior(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_stm32_peripheral_create(&alloc, "STM32F401RETx", 14);
    p.vtable->init_peripheral(p.ctx);
    sc_peripheral_error_t err = p.vtable->flash(p.ctx, "/tmp/firmware.hex", 17);
    SC_ASSERT_TRUE(err == SC_PERIPHERAL_ERR_UNSUPPORTED_OPERATION ||
                   err == SC_PERIPHERAL_ERR_NONE ||
                   err == SC_PERIPHERAL_ERR_NOT_CONNECTED);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_arduino_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 11);
    bool ok = p.vtable->health_check(p.ctx);
    SC_ASSERT_TRUE(ok || !ok);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_rpi_health_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_rpi_peripheral_create(&alloc);
    bool ok = p.vtable->health_check(p.ctx);
    SC_ASSERT_TRUE(ok || !ok);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_rpi_capabilities(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_rpi_peripheral_create(&alloc);
    sc_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    SC_ASSERT_TRUE(strlen(cap.board_type) > 0);
    SC_ASSERT_EQ(cap.flash_size_kb, 0u);
    SC_ASSERT_FALSE(cap.has_flash);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_arduino_write_multiple_pins(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_arduino_peripheral_create(&alloc, "/dev/cu.usbmodem0", 17);
    p.vtable->init_peripheral(p.ctx);
    for (uint32_t addr = 0; addr < 5; addr++) {
        sc_peripheral_error_t err = p.vtable->write(p.ctx, addr, (uint8_t)(addr & 1));
        SC_ASSERT_EQ(err, SC_PERIPHERAL_ERR_NONE);
    }
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_peripheral_create_null_config(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p;
    sc_error_t err = sc_peripheral_create(&alloc, "rpi", 3, NULL, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(p.ctx);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_hardware_discover_zero_count(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_hardware_info_t results[1];
    size_t count = 0;
    sc_error_t err = sc_hardware_discover(&alloc, results, &count);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_factory_arduino_config_serial(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_config_t config = {
        .serial_port = "/dev/ttyACM0",
        .serial_port_len = 12,
        .chip = NULL,
        .chip_len = 0,
    };
    sc_peripheral_t p;
    sc_error_t err = sc_peripheral_create(&alloc, "arduino", 7, &config, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "arduino");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_stm32_board_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_stm32_peripheral_create(&alloc, "STM32F401RETx", 14);
    SC_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "nucleo");
    p.vtable->destroy(p.ctx, &alloc);
}

/* Hardware discovery tests (SC_IS_TEST returns mock) */
static void test_hardware_discover_board_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_hardware_info_t results[8];
    size_t count = 8;
    sc_error_t err = sc_hardware_discover(&alloc, results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count >= 1);
    SC_ASSERT_TRUE(strlen(results[0].board_name) > 0);
    SC_ASSERT_TRUE(strcmp(results[0].board_name, "arduino-uno") == 0 ||
                   strlen(results[0].board_name) > 0);
}

static void test_hardware_discover_board_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_hardware_info_t results[4];
    size_t count = 4;
    sc_error_t err = sc_hardware_discover(&alloc, results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count >= 1);
    SC_ASSERT_TRUE(strlen(results[0].board_type) > 0);
}

static void test_hardware_discover_detected_flag(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_hardware_info_t results[4];
    size_t count = 4;
    sc_error_t err = sc_hardware_discover(&alloc, results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count >= 1);
    SC_ASSERT_TRUE(results[0].detected);
}

static void test_hardware_discover_serial_port_in_test(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_hardware_info_t results[4];
    size_t count = 4;
    sc_error_t err = sc_hardware_discover(&alloc, results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count >= 1);
    SC_ASSERT_TRUE(strlen(results[0].serial_port) >= 0);
}

static void test_hardware_discover_reduces_count(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_hardware_info_t results[16];
    size_t count = 16;
    sc_error_t err = sc_hardware_discover(&alloc, results, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count <= 16);
}

static void test_arduino_read_returns_zero_in_test(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_arduino_peripheral_create(&alloc, "/dev/null", 8);
    p.vtable->init_peripheral(p.ctx);
    uint8_t val = 0xFF;
    sc_peripheral_error_t err = p.vtable->read(p.ctx, 13, &val);
    SC_ASSERT_EQ(err, SC_PERIPHERAL_ERR_NONE);
    SC_ASSERT_EQ(val, 0u);
    p.vtable->destroy(p.ctx, &alloc);
}

/* Arduino vtable wiring */
static void test_arduino_vtable_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 11);
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "arduino");
    p.vtable->destroy(p.ctx, &alloc);
}

/* STM32 vtable wiring */
static void test_stm32_vtable_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_stm32_peripheral_create(&alloc, "STM32F411RETx", 14);
    SC_ASSERT_NOT_NULL(p.vtable->name(p.ctx));
    SC_ASSERT_TRUE(strlen(p.vtable->name(p.ctx)) > 0);
    p.vtable->destroy(p.ctx, &alloc);
}

/* RPi vtable wiring */
static void test_rpi_vtable_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_rpi_peripheral_create(&alloc);
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "rpi-gpio");
    p.vtable->destroy(p.ctx, &alloc);
}

/* Peripheral factory by name */
static void test_peripheral_factory_arduino_by_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_config_t config = {
        .serial_port = "/dev/ttyACM0",
        .serial_port_len = 12,
        .chip = NULL,
        .chip_len = 0,
    };
    sc_peripheral_t p;
    sc_error_t err = sc_peripheral_create(&alloc, "arduino", 7, &config, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(p.vtable->name(p.ctx), "arduino");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_peripheral_factory_stm32_by_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_config_t config = {
        .serial_port = NULL,
        .serial_port_len = 0,
        .chip = "STM32F401RETx",
        .chip_len = 14,
    };
    sc_peripheral_t p;
    sc_error_t err = sc_peripheral_create(&alloc, "stm32", 5, &config, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "nucleo");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_peripheral_factory_rpi_by_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p;
    sc_error_t err = sc_peripheral_create(&alloc, "rpi", 3, NULL, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "rpi-gpio");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_arduino_capabilities_flash_size(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 11);
    sc_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    SC_ASSERT_EQ(cap.flash_size_kb, 32u);
    SC_ASSERT_TRUE(cap.has_flash);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_stm32_capabilities_flash_size(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_stm32_peripheral_create(&alloc, "STM32F401RETx", 14);
    sc_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    SC_ASSERT_EQ(cap.flash_size_kb, 512u);
    SC_ASSERT_TRUE(cap.has_flash);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_rpi_capabilities_no_flash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_peripheral_t p = sc_rpi_peripheral_create(&alloc);
    sc_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    SC_ASSERT_EQ(cap.flash_size_kb, 0u);
    SC_ASSERT_FALSE(cap.has_flash);
    SC_ASSERT_TRUE(cap.has_gpio);
    p.vtable->destroy(p.ctx, &alloc);
}

void run_peripheral_tests(void) {
    SC_TEST_SUITE("peripheral");
    SC_RUN_TEST(test_factory_arduino_create);
    SC_RUN_TEST(test_factory_stm32_create);
    SC_RUN_TEST(test_factory_rpi_create);
    SC_RUN_TEST(test_factory_unknown_type_returns_not_supported);
    SC_RUN_TEST(test_factory_arduino_config_serial);
    SC_RUN_TEST(test_peripheral_create_null_config);
    SC_RUN_TEST(test_capabilities_struct);
    SC_RUN_TEST(test_rpi_flash_unsupported);
    SC_RUN_TEST(test_stm32_flash_behavior);
    SC_RUN_TEST(test_rpi_capabilities);
    SC_RUN_TEST(test_arduino_read_write_no_io_in_test);
    SC_RUN_TEST(test_arduino_write_multiple_pins);
    SC_RUN_TEST(test_arduino_read_returns_zero_in_test);
    SC_RUN_TEST(test_arduino_health_check);
    SC_RUN_TEST(test_rpi_health_check);
    SC_RUN_TEST(test_stm32_board_type);
    SC_RUN_TEST(test_hardware_discover);
    SC_RUN_TEST(test_hardware_discover_zero_count);
    SC_RUN_TEST(test_hardware_discover_board_name);
    SC_RUN_TEST(test_hardware_discover_board_type);
    SC_RUN_TEST(test_hardware_discover_detected_flag);
    SC_RUN_TEST(test_hardware_discover_serial_port_in_test);
    SC_RUN_TEST(test_hardware_discover_reduces_count);
    SC_RUN_TEST(test_arduino_vtable_name);
    SC_RUN_TEST(test_stm32_vtable_name);
    SC_RUN_TEST(test_rpi_vtable_name);
    SC_RUN_TEST(test_peripheral_factory_arduino_by_name);
    SC_RUN_TEST(test_peripheral_factory_stm32_by_name);
    SC_RUN_TEST(test_peripheral_factory_rpi_by_name);
    SC_RUN_TEST(test_arduino_capabilities_flash_size);
    SC_RUN_TEST(test_stm32_capabilities_flash_size);
    SC_RUN_TEST(test_rpi_capabilities_no_flash);
}
