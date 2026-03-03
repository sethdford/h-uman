/* Fuzz harness for sc_json_parse and JSON accessors.
 * Must not crash on any input. */
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536)
        return 0;

    sc_allocator_t alloc = sc_system_allocator();
    char *buf = (char *)alloc.alloc(alloc.ctx, size + 1);
    if (!buf)
        return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, buf, size, &val);
    alloc.free(alloc.ctx, buf, size + 1);

    if (err != SC_OK) {
        return 0;
    }
    if (!val) {
        return 0;
    }

    /* Exercise accessors based on value type */
    switch (val->type) {
    case SC_JSON_OBJECT:
        for (size_t i = 0; i < val->data.object.len; i++) {
            sc_json_pair_t *pair = &val->data.object.pairs[i];
            if (pair->key && pair->key_len > 0) {
                (void)sc_json_object_get(val, pair->key);
                (void)sc_json_get_string(val, pair->key);
                (void)sc_json_get_number(val, pair->key, 0.0);
                (void)sc_json_get_bool(val, pair->key, false);
            }
        }
        break;
    case SC_JSON_ARRAY:
        for (size_t i = 0; i < val->data.array.len; i++) {
            sc_json_value_t *item = val->data.array.items[i];
            if (item)
                (void)item->type;
        }
        break;
    case SC_JSON_STRING:
        if (val->data.string.ptr && val->data.string.len > 0)
            (void)val->data.string.ptr[0];
        break;
    case SC_JSON_NUMBER:
        (void)val->data.number;
        break;
    case SC_JSON_BOOL:
        (void)val->data.boolean;
        break;
    case SC_JSON_NULL:
        break;
    }

    sc_json_free(&alloc, val);
    return 0;
}
