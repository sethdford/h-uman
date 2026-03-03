#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include <string.h>

static sc_error_t migrate_v1_to_v2(sc_allocator_t *alloc, sc_json_value_t *root) {
    /* v1→v2: if top-level "memory_backend" exists, wrap it into memory.backend */
    const char *mb = sc_json_get_string(root, "memory_backend");
    if (mb) {
        sc_json_value_t *mem = sc_json_object_new(alloc);
        if (!mem)
            return SC_ERR_OUT_OF_MEMORY;
        sc_json_object_set(alloc, mem, "backend",
                           sc_json_string_new(alloc, mb, strlen(mb)));
        sc_json_object_set(alloc, root, "memory", mem);
    }
    return SC_OK;
}

sc_error_t sc_config_migrate(sc_allocator_t *alloc, sc_json_value_t *root) {
    if (!alloc || !root)
        return SC_ERR_INVALID_ARGUMENT;

    double ver = sc_json_get_number(root, "config_version", 1.0);
    int version = (int)ver;

    if (version > SC_CONFIG_VERSION_CURRENT)
        return SC_ERR_INVALID_ARGUMENT; /* future version — reject */

    if (version < 1)
        version = 1;

    if (version < 2) {
        sc_error_t err = migrate_v1_to_v2(alloc, root);
        if (err != SC_OK)
            return err;
    }

    /* Stamp current version */
    sc_json_object_set(alloc, root, "config_version",
                       sc_json_number_new(alloc, (double)SC_CONFIG_VERSION_CURRENT));

    return SC_OK;
}
