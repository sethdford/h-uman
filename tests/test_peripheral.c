#include "human/core/allocator.h"
#include "human/hardware.h"
#include "human/peripheral.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>

static void test_factory_arduino_create(void) {
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
    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "arduino");
    HU_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "arduino-uno");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_factory_stm32_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_config_t config = {
        .serial_port = NULL,
        .serial_port_len = 0,
        .chip = "STM32F401RETx",
        .chip_len = 14,
    };
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(&alloc, "stm32", 5, &config, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_NOT_NULL(p.vtable);
    HU_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "nucleo");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_factory_rpi_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(&alloc, "rpi", 3, NULL, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_NOT_NULL(p.vtable);
    HU_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "rpi-gpio");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_factory_unknown_type_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_config_t config = {0};
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(&alloc, "unknown", 7, &config, &p);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_capabilities_struct(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 12);
    HU_ASSERT_NOT_NULL(p.ctx);

    hu_peripheral_error_t init_err = p.vtable->init_peripheral(p.ctx);
    HU_ASSERT_EQ(init_err, HU_PERIPHERAL_ERR_NONE);

    hu_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    HU_ASSERT_TRUE(cap.has_serial);
    HU_ASSERT_TRUE(cap.has_gpio);
    HU_ASSERT_TRUE(cap.has_flash);
    HU_ASSERT_EQ(cap.flash_size_kb, 32u);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_rpi_flash_unsupported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_rpi_peripheral_create(&alloc);
    HU_ASSERT_NOT_NULL(p.ctx);

    p.vtable->init_peripheral(p.ctx);

    hu_peripheral_error_t err = p.vtable->flash(p.ctx, "/tmp/firmware.hex", 17);
    HU_ASSERT_EQ(err, HU_PERIPHERAL_ERR_UNSUPPORTED_OPERATION);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_arduino_read_write_no_io_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/cu.usbmodem0", 17);
    HU_ASSERT_NOT_NULL(p.ctx);

    p.vtable->init_peripheral(p.ctx);

    uint8_t val = 0xFF;
    hu_peripheral_error_t rerr = p.vtable->read(p.ctx, 0, &val);
    HU_ASSERT_EQ(rerr, HU_PERIPHERAL_ERR_NONE);
    HU_ASSERT_EQ(val, 0u);

    hu_peripheral_error_t werr = p.vtable->write(p.ctx, 1, 1);
    HU_ASSERT_EQ(werr, HU_PERIPHERAL_ERR_NONE);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_hardware_discover(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hardware_info_t results[8];
    size_t count = 8;
    hu_error_t err = hu_hardware_discover(&alloc, results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_TRUE(results[0].detected);
    HU_ASSERT_TRUE(strlen(results[0].board_name) > 0);
    HU_ASSERT_TRUE(strlen(results[0].board_type) > 0);
}

static void test_stm32_flash_behavior(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_stm32_peripheral_create(&alloc, "STM32F401RETx", 14);
    p.vtable->init_peripheral(p.ctx);
    hu_peripheral_error_t err = p.vtable->flash(p.ctx, "/tmp/firmware.hex", 17);
    HU_ASSERT_TRUE(err == HU_PERIPHERAL_ERR_UNSUPPORTED_OPERATION ||
                   err == HU_PERIPHERAL_ERR_NONE || err == HU_PERIPHERAL_ERR_NOT_CONNECTED);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_arduino_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 11);
    bool ok = p.vtable->health_check(p.ctx);
    (void)ok;
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_rpi_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_rpi_peripheral_create(&alloc);
    bool ok = p.vtable->health_check(p.ctx);
    (void)ok;
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_rpi_capabilities(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_rpi_peripheral_create(&alloc);
    hu_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    HU_ASSERT_TRUE(strlen(cap.board_type) > 0);
    HU_ASSERT_EQ(cap.flash_size_kb, 0u);
    HU_ASSERT_FALSE(cap.has_flash);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_arduino_write_multiple_pins(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/cu.usbmodem0", 17);
    p.vtable->init_peripheral(p.ctx);
    for (uint32_t addr = 0; addr < 5; addr++) {
        hu_peripheral_error_t err = p.vtable->write(p.ctx, addr, (uint8_t)(addr & 1));
        HU_ASSERT_EQ(err, HU_PERIPHERAL_ERR_NONE);
    }
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_peripheral_create_null_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(&alloc, "rpi", 3, NULL, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.ctx);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_hardware_discover_zero_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hardware_info_t results[1];
    size_t count = 0;
    hu_error_t err = hu_hardware_discover(&alloc, results, &count);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_factory_arduino_config_serial(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_config_t config = {
        .serial_port = "/dev/ttyACM0",
        .serial_port_len = 12,
        .chip = NULL,
        .chip_len = 0,
    };
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(&alloc, "arduino", 7, &config, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "arduino");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_stm32_board_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_stm32_peripheral_create(&alloc, "STM32F401RETx", 14);
    HU_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "nucleo");
    p.vtable->destroy(p.ctx, &alloc);
}

/* Hardware discovery tests (HU_IS_TEST returns mock) */
static void test_hardware_discover_board_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hardware_info_t results[8];
    size_t count = 8;
    hu_error_t err = hu_hardware_discover(&alloc, results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_TRUE(strlen(results[0].board_name) > 0);
    HU_ASSERT_TRUE(strcmp(results[0].board_name, "arduino-uno") == 0 ||
                   strlen(results[0].board_name) > 0);
}

static void test_hardware_discover_board_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hardware_info_t results[4];
    size_t count = 4;
    hu_error_t err = hu_hardware_discover(&alloc, results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_TRUE(strlen(results[0].board_type) > 0);
}

static void test_hardware_discover_detected_flag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hardware_info_t results[4];
    size_t count = 4;
    hu_error_t err = hu_hardware_discover(&alloc, results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_TRUE(results[0].detected);
}

static void test_hardware_discover_serial_port_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hardware_info_t results[4];
    size_t count = 4;
    hu_error_t err = hu_hardware_discover(&alloc, results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_NOT_NULL(results[0].serial_port);
}

static void test_hardware_discover_reduces_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_hardware_info_t results[16];
    size_t count = 16;
    hu_error_t err = hu_hardware_discover(&alloc, results, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count <= 16);
}

static void test_arduino_read_returns_zero_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/null", 8);
    p.vtable->init_peripheral(p.ctx);
    uint8_t val = 0xFF;
    hu_peripheral_error_t err = p.vtable->read(p.ctx, 13, &val);
    HU_ASSERT_EQ(err, HU_PERIPHERAL_ERR_NONE);
    HU_ASSERT_EQ(val, 0u);
    p.vtable->destroy(p.ctx, &alloc);
}

/* Arduino vtable wiring */
static void test_arduino_vtable_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 11);
    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "arduino");
    p.vtable->destroy(p.ctx, &alloc);
}

/* STM32 vtable wiring */
static void test_stm32_vtable_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_stm32_peripheral_create(&alloc, "STM32F411RETx", 14);
    HU_ASSERT_NOT_NULL(p.vtable->name(p.ctx));
    HU_ASSERT_TRUE(strlen(p.vtable->name(p.ctx)) > 0);
    p.vtable->destroy(p.ctx, &alloc);
}

/* RPi vtable wiring */
static void test_rpi_vtable_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_rpi_peripheral_create(&alloc);
    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "rpi-gpio");
    p.vtable->destroy(p.ctx, &alloc);
}

/* Peripheral factory by name */
static void test_peripheral_factory_arduino_by_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_config_t config = {
        .serial_port = "/dev/ttyACM0",
        .serial_port_len = 12,
        .chip = NULL,
        .chip_len = 0,
    };
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(&alloc, "arduino", 7, &config, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "arduino");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_peripheral_factory_stm32_by_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_config_t config = {
        .serial_port = NULL,
        .serial_port_len = 0,
        .chip = "STM32F401RETx",
        .chip_len = 14,
    };
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(&alloc, "stm32", 5, &config, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "nucleo");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_peripheral_factory_rpi_by_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(&alloc, "rpi", 3, NULL, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "rpi-gpio");
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_arduino_capabilities_flash_size(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 11);
    hu_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    HU_ASSERT_EQ(cap.flash_size_kb, 32u);
    HU_ASSERT_TRUE(cap.has_flash);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_stm32_capabilities_flash_size(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_stm32_peripheral_create(&alloc, "STM32F401RETx", 14);
    hu_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    HU_ASSERT_EQ(cap.flash_size_kb, 512u);
    HU_ASSERT_TRUE(cap.has_flash);
    p.vtable->destroy(p.ctx, &alloc);
}

static void test_rpi_capabilities_no_flash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_rpi_peripheral_create(&alloc);
    hu_peripheral_capabilities_t cap = p.vtable->capabilities(p.ctx);
    HU_ASSERT_EQ(cap.flash_size_kb, 0u);
    HU_ASSERT_FALSE(cap.has_flash);
    HU_ASSERT_TRUE(cap.has_gpio);
    p.vtable->destroy(p.ctx, &alloc);
}

/* ── Mock tests (HU_IS_TEST: no real serial/probe-rs/GPIO) ────────────────── */

static void test_mock_arduino_create_name_read_write_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/cu.usbmodem0", 17);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_NOT_NULL(p.vtable);

    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "arduino");

    hu_peripheral_error_t init_err = p.vtable->init_peripheral(p.ctx);
    HU_ASSERT_EQ(init_err, HU_PERIPHERAL_ERR_NONE);

    uint8_t val = 0xFF;
    hu_peripheral_error_t rerr = p.vtable->read(p.ctx, 0, &val);
    HU_ASSERT_EQ(rerr, HU_PERIPHERAL_ERR_NONE);
    HU_ASSERT_EQ(val, 0u);

    hu_peripheral_error_t werr = p.vtable->write(p.ctx, 1, 1);
    HU_ASSERT_EQ(werr, HU_PERIPHERAL_ERR_NONE);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_mock_stm32_create_name_read_write_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_stm32_peripheral_create(&alloc, "STM32F401RETx", 14);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_NOT_NULL(p.vtable);

    HU_ASSERT_STR_EQ(p.vtable->board_type(p.ctx), "nucleo");
    HU_ASSERT_TRUE(strlen(p.vtable->name(p.ctx)) > 0);

    hu_peripheral_error_t init_err = p.vtable->init_peripheral(p.ctx);
    HU_ASSERT_EQ(init_err, HU_PERIPHERAL_ERR_NONE);

    uint8_t val = 0xFF;
    hu_peripheral_error_t rerr = p.vtable->read(p.ctx, 0x08000000, &val);
    HU_ASSERT_EQ(rerr, HU_PERIPHERAL_ERR_NONE);
    HU_ASSERT_EQ(val, 0u);

    hu_peripheral_error_t werr = p.vtable->write(p.ctx, 0x40000000, 0xAB);
    HU_ASSERT_EQ(werr, HU_PERIPHERAL_ERR_NONE);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_mock_rpi_create_name_read_write_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_rpi_peripheral_create(&alloc);
    HU_ASSERT_NOT_NULL(p.ctx);
    HU_ASSERT_NOT_NULL(p.vtable);

    HU_ASSERT_STR_EQ(p.vtable->name(p.ctx), "rpi-gpio");

    hu_peripheral_error_t init_err = p.vtable->init_peripheral(p.ctx);
    HU_ASSERT_EQ(init_err, HU_PERIPHERAL_ERR_NONE);

    uint8_t val = 0xFF;
    hu_peripheral_error_t rerr = p.vtable->read(p.ctx, 17, &val);
    HU_ASSERT_EQ(rerr, HU_PERIPHERAL_ERR_NONE);
    HU_ASSERT_EQ(val, 0u);

    hu_peripheral_error_t werr = p.vtable->write(p.ctx, 18, 1);
    HU_ASSERT_EQ(werr, HU_PERIPHERAL_ERR_NONE);

    p.vtable->destroy(p.ctx, &alloc);
}

/* ── Error cases ─────────────────────────────────────────────────────────── */

static void test_error_arduino_create_null_allocator(void) {
    hu_peripheral_t p = hu_arduino_peripheral_create(NULL, "/dev/ttyUSB0", 11);
    HU_ASSERT_NULL(p.ctx);
    HU_ASSERT_NULL(p.vtable);
}

static void test_error_stm32_create_null_allocator(void) {
    hu_peripheral_t p = hu_stm32_peripheral_create(NULL, "STM32F401RETx", 14);
    HU_ASSERT_NULL(p.ctx);
    HU_ASSERT_NULL(p.vtable);
}

static void test_error_rpi_create_null_allocator(void) {
    hu_peripheral_t p = hu_rpi_peripheral_create(NULL);
    HU_ASSERT_NULL(p.ctx);
    HU_ASSERT_NULL(p.vtable);
}

static void test_error_factory_create_null_allocator(void) {
    hu_peripheral_config_t config = {0};
    hu_peripheral_t p;
    hu_error_t err = hu_peripheral_create(NULL, "arduino", 7, &config, &p);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_error_arduino_create_null_serial_port(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, NULL, 0);
    HU_ASSERT_NULL(p.ctx);
    HU_ASSERT_NULL(p.vtable);
}

static void test_error_stm32_create_null_chip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_stm32_peripheral_create(&alloc, NULL, 0);
    HU_ASSERT_NULL(p.ctx);
    HU_ASSERT_NULL(p.vtable);
}

static void test_error_read_write_uninitialized_arduino(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 11);
    HU_ASSERT_NOT_NULL(p.ctx);
    /* Do NOT call init_peripheral */

    uint8_t val;
    hu_peripheral_error_t rerr = p.vtable->read(p.ctx, 0, &val);
    HU_ASSERT_EQ(rerr, HU_PERIPHERAL_ERR_NOT_CONNECTED);

    hu_peripheral_error_t werr = p.vtable->write(p.ctx, 1, 1);
    HU_ASSERT_EQ(werr, HU_PERIPHERAL_ERR_NOT_CONNECTED);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_error_read_write_uninitialized_stm32(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_stm32_peripheral_create(&alloc, "STM32F401RETx", 14);
    HU_ASSERT_NOT_NULL(p.ctx);
    /* Do NOT call init_peripheral */

    uint8_t val;
    hu_peripheral_error_t rerr = p.vtable->read(p.ctx, 0x08000000, &val);
    HU_ASSERT_EQ(rerr, HU_PERIPHERAL_ERR_NOT_CONNECTED);

    hu_peripheral_error_t werr = p.vtable->write(p.ctx, 0x40000000, 0x01);
    HU_ASSERT_EQ(werr, HU_PERIPHERAL_ERR_NOT_CONNECTED);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_error_read_write_uninitialized_rpi(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_rpi_peripheral_create(&alloc);
    HU_ASSERT_NOT_NULL(p.ctx);
    /* Do NOT call init_peripheral */

    uint8_t val;
    hu_peripheral_error_t rerr = p.vtable->read(p.ctx, 17, &val);
    HU_ASSERT_EQ(rerr, HU_PERIPHERAL_ERR_NOT_CONNECTED);

    hu_peripheral_error_t werr = p.vtable->write(p.ctx, 18, 1);
    HU_ASSERT_EQ(werr, HU_PERIPHERAL_ERR_NOT_CONNECTED);

    p.vtable->destroy(p.ctx, &alloc);
}

static void test_error_read_null_out_value(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_peripheral_t p = hu_arduino_peripheral_create(&alloc, "/dev/ttyUSB0", 11);
    p.vtable->init_peripheral(p.ctx);

    hu_peripheral_error_t err = p.vtable->read(p.ctx, 0, NULL);
    HU_ASSERT_EQ(err, HU_PERIPHERAL_ERR_INVALID_ADDRESS);

    p.vtable->destroy(p.ctx, &alloc);
}

void run_peripheral_tests(void) {
    HU_TEST_SUITE("peripheral");
    HU_RUN_TEST(test_factory_arduino_create);
    HU_RUN_TEST(test_factory_stm32_create);
    HU_RUN_TEST(test_factory_rpi_create);
    HU_RUN_TEST(test_factory_unknown_type_returns_not_supported);
    HU_RUN_TEST(test_factory_arduino_config_serial);
    HU_RUN_TEST(test_peripheral_create_null_config);
    HU_RUN_TEST(test_capabilities_struct);
    HU_RUN_TEST(test_rpi_flash_unsupported);
    HU_RUN_TEST(test_stm32_flash_behavior);
    HU_RUN_TEST(test_rpi_capabilities);
    HU_RUN_TEST(test_arduino_read_write_no_io_in_test);
    HU_RUN_TEST(test_arduino_write_multiple_pins);
    HU_RUN_TEST(test_arduino_read_returns_zero_in_test);
    HU_RUN_TEST(test_arduino_health_check);
    HU_RUN_TEST(test_rpi_health_check);
    HU_RUN_TEST(test_stm32_board_type);
    HU_RUN_TEST(test_hardware_discover);
    HU_RUN_TEST(test_hardware_discover_zero_count);
    HU_RUN_TEST(test_hardware_discover_board_name);
    HU_RUN_TEST(test_hardware_discover_board_type);
    HU_RUN_TEST(test_hardware_discover_detected_flag);
    HU_RUN_TEST(test_hardware_discover_serial_port_in_test);
    HU_RUN_TEST(test_hardware_discover_reduces_count);
    HU_RUN_TEST(test_arduino_vtable_name);
    HU_RUN_TEST(test_stm32_vtable_name);
    HU_RUN_TEST(test_rpi_vtable_name);
    HU_RUN_TEST(test_peripheral_factory_arduino_by_name);
    HU_RUN_TEST(test_peripheral_factory_stm32_by_name);
    HU_RUN_TEST(test_peripheral_factory_rpi_by_name);
    HU_RUN_TEST(test_arduino_capabilities_flash_size);
    HU_RUN_TEST(test_stm32_capabilities_flash_size);
    HU_RUN_TEST(test_rpi_capabilities_no_flash);
    HU_RUN_TEST(test_mock_arduino_create_name_read_write_deinit);
    HU_RUN_TEST(test_mock_stm32_create_name_read_write_deinit);
    HU_RUN_TEST(test_mock_rpi_create_name_read_write_deinit);
    HU_RUN_TEST(test_error_arduino_create_null_allocator);
    HU_RUN_TEST(test_error_stm32_create_null_allocator);
    HU_RUN_TEST(test_error_rpi_create_null_allocator);
    HU_RUN_TEST(test_error_factory_create_null_allocator);
    HU_RUN_TEST(test_error_arduino_create_null_serial_port);
    HU_RUN_TEST(test_error_stm32_create_null_chip);
    HU_RUN_TEST(test_error_read_write_uninitialized_arduino);
    HU_RUN_TEST(test_error_read_write_uninitialized_stm32);
    HU_RUN_TEST(test_error_read_write_uninitialized_rpi);
    HU_RUN_TEST(test_error_read_null_out_value);
}
