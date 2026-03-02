#include "seaclaw/hardware.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef SC_IS_TEST
#ifdef __linux__
#include <dirent.h>
#include <sys/stat.h>
#endif
#ifdef __APPLE__
#include <dirent.h>
#include <sys/stat.h>
#endif
#endif

#ifndef SC_IS_TEST
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <fcntl.h>
#include <unistd.h>
#endif
#endif

sc_error_t sc_hardware_discover(sc_allocator_t *alloc,
    sc_hardware_info_t *results,
    size_t *count)
{
    if (!alloc || !results || !count) return SC_ERR_INVALID_ARGUMENT;
    size_t max_count = *count;
    size_t found = 0;

#ifndef SC_IS_TEST
#ifdef __linux__
    /* Scan /dev for ttyUSB*, ttyACM* */
    const char *dev_prefixes[] = { "/dev/ttyUSB", "/dev/ttyACM" };
    for (size_t p = 0; p < sizeof(dev_prefixes) / sizeof(dev_prefixes[0]) && found < max_count; p++) {
        for (int i = 0; i < 16 && found < max_count; i++) {
            char path[64];
            snprintf(path, sizeof(path), "%s%d", dev_prefixes[p], i);
            struct stat st;
            if (stat(path, &st) == 0) {
                sc_hardware_info_t *r = &results[found];
                strncpy(r->board_name, "arduino", sizeof(r->board_name) - 1);
                r->board_name[sizeof(r->board_name) - 1] = '\0';
                strncpy(r->board_type, "arduino", sizeof(r->board_type) - 1);
                r->board_type[sizeof(r->board_type) - 1] = '\0';
                strncpy(r->serial_port, path, sizeof(r->serial_port) - 1);
                r->serial_port[sizeof(r->serial_port) - 1] = '\0';
                r->detected = true;
                found++;
            }
        }
    }

    /* Check for probe-rs (STM32) */
    if (system("probe-rs --version >/dev/null 2>&1") == 0 && found < max_count) {
        sc_hardware_info_t *r = &results[found];
        strncpy(r->board_name, "STM32F401RETx", sizeof(r->board_name) - 1);
        r->board_name[sizeof(r->board_name) - 1] = '\0';
        strncpy(r->board_type, "nucleo", sizeof(r->board_type) - 1);
        r->board_type[sizeof(r->board_type) - 1] = '\0';
        r->serial_port[0] = '\0';
        r->detected = true;
        found++;
    }

    /* Check for RPi GPIO */
    int gpio_fd = open("/sys/class/gpio", O_RDONLY);
    if (gpio_fd >= 0) {
        close(gpio_fd);
        if (found < max_count) {
            sc_hardware_info_t *r = &results[found];
            strncpy(r->board_name, "rpi-gpio", sizeof(r->board_name) - 1);
            r->board_name[sizeof(r->board_name) - 1] = '\0';
            strncpy(r->board_type, "rpi-gpio", sizeof(r->board_type) - 1);
            r->board_type[sizeof(r->board_type) - 1] = '\0';
            r->serial_port[0] = '\0';
            r->detected = true;
            found++;
        }
    }
#endif
#ifdef __APPLE__
    /* macOS: /dev/cu.usbmodem* for Arduino */
    const char *dev_prefixes[] = { "/dev/cu.usbmodem", "/dev/cu.usbserial" };
    for (size_t p = 0; p < sizeof(dev_prefixes) / sizeof(dev_prefixes[0]) && found < max_count; p++) {
        for (int i = 0; i < 16 && found < max_count; i++) {
            char path[64];
            snprintf(path, sizeof(path), "%s%d", dev_prefixes[p], i);
            struct stat st;
            if (stat(path, &st) == 0) {
                sc_hardware_info_t *r = &results[found];
                strncpy(r->board_name, "arduino", sizeof(r->board_name) - 1);
                r->board_name[sizeof(r->board_name) - 1] = '\0';
                strncpy(r->board_type, "arduino", sizeof(r->board_type) - 1);
                r->board_type[sizeof(r->board_type) - 1] = '\0';
                strncpy(r->serial_port, path, sizeof(r->serial_port) - 1);
                r->serial_port[sizeof(r->serial_port) - 1] = '\0';
                r->detected = true;
                found++;
            }
        }
    }
    /* RPi GPIO and STM32 not supported on macOS */
#endif
#if !defined(__linux__) && !defined(__APPLE__)
    (void)alloc;
#endif
#else
    /* Test mode: return mock hardware */
    if (max_count >= 1) {
        strncpy(results[0].board_name, "arduino-uno", sizeof(results[0].board_name) - 1);
        results[0].board_name[sizeof(results[0].board_name) - 1] = '\0';
        strncpy(results[0].board_type, "arduino", sizeof(results[0].board_type) - 1);
        results[0].board_type[sizeof(results[0].board_type) - 1] = '\0';
        strncpy(results[0].serial_port, "/dev/cu.usbmodem0", sizeof(results[0].serial_port) - 1);
        results[0].serial_port[sizeof(results[0].serial_port) - 1] = '\0';
        results[0].detected = true;
        found = 1;
    }
#endif

    *count = found;
    return SC_OK;
}
