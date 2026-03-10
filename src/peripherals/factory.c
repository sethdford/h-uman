#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/peripheral.h"
#include <string.h>

hu_error_t hu_peripheral_create(hu_allocator_t *alloc, const char *type, size_t type_len,
                                const hu_peripheral_config_t *config, hu_peripheral_t *out) {
    if (!alloc || !type || !out)
        return HU_ERR_INVALID_ARGUMENT;

    if (type_len == 3 && memcmp(type, "rpi", 3) == 0) {
        *out = hu_rpi_peripheral_create(alloc);
        return out->ctx ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }

    if (!config)
        return HU_ERR_NOT_SUPPORTED;

    if (type_len == 7 && memcmp(type, "arduino", 7) == 0) {
        const char *port = config->serial_port;
        size_t port_len = config->serial_port_len;
        if (!port || port_len == 0)
            return HU_ERR_INVALID_ARGUMENT;
        *out = hu_arduino_peripheral_create(alloc, port, port_len);
        return out->ctx ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }

    if (type_len == 5 && memcmp(type, "stm32", 5) == 0) {
        const char *chip = config->chip;
        size_t chip_len = config->chip_len;
        if (!chip || chip_len == 0) {
            chip = "STM32F401RETx";
            chip_len = 14;
        }
        *out = hu_stm32_peripheral_create(alloc, chip, chip_len);
        return out->ctx ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }

    return HU_ERR_NOT_SUPPORTED;
}
