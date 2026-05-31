#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <string.h>

static hu_error_t migrate_v1_to_v2(hu_allocator_t *alloc, hu_json_value_t *root) {
    /* v1→v2: if top-level "memory_backend" exists, wrap it into memory.backend */
    const char *mb = hu_json_get_string(root, "memory_backend");
    if (mb) {
        hu_json_value_t *mem = hu_json_object_new(alloc);
        if (!mem)
            return HU_ERR_OUT_OF_MEMORY;
        hu_json_object_set(alloc, mem, "backend", hu_json_string_new(alloc, mb, strlen(mb)));
        hu_json_object_set(alloc, root, "memory", mem);
    }
    return HU_OK;
}

hu_error_t hu_config_migrate(hu_allocator_t *alloc, hu_json_value_t *root) {
    if (!alloc || !root)
        return HU_ERR_INVALID_ARGUMENT;

    double ver = hu_json_get_number(root, "config_version", 1.0);
    int version = (int)ver;

    if (version > HU_CONFIG_VERSION_CURRENT)
        return HU_ERR_INVALID_ARGUMENT; /* future version — reject */

    if (version < 1)
        version = 1;

    if (version < 2) {
        hu_error_t err = migrate_v1_to_v2(alloc, root);
        if (err != HU_OK)
            return err;
    }

    /* Stamp current version */
    hu_json_object_set(alloc, root, "config_version",
                       hu_json_number_new(alloc, (double)HU_CONFIG_VERSION_CURRENT));

    return HU_OK;
}
