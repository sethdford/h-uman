#include "human/json_util.h"
#include <stdint.h>
#include <string.h>

hu_error_t hu_json_util_append_string(hu_json_buf_t *buf, const char *s) {
    if (!buf || !s)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_json_append_string(buf, s, strlen(s));
}

hu_error_t hu_json_util_append_key(hu_json_buf_t *buf, const char *key) {
    if (!buf || !key)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_json_append_key(buf, key, strlen(key));
}

hu_error_t hu_json_util_append_key_value(hu_json_buf_t *buf, const char *key, const char *value) {
    if (!buf || !key || !value)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_json_append_key_value(buf, key, strlen(key), value, strlen(value));
}

hu_error_t hu_json_util_append_key_int(hu_json_buf_t *buf, const char *key, int64_t value) {
    if (!buf || !key)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_json_append_key_int(buf, key, strlen(key), (long long)value);
}
