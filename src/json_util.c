#include "seaclaw/json_util.h"
#include <stdint.h>
#include <string.h>

sc_error_t sc_json_util_append_string(sc_json_buf_t *buf, const char *s) {
    if (!buf || !s)
        return SC_ERR_INVALID_ARGUMENT;
    return sc_json_append_string(buf, s, strlen(s));
}

sc_error_t sc_json_util_append_key(sc_json_buf_t *buf, const char *key) {
    if (!buf || !key)
        return SC_ERR_INVALID_ARGUMENT;
    return sc_json_append_key(buf, key, strlen(key));
}

sc_error_t sc_json_util_append_key_value(sc_json_buf_t *buf, const char *key, const char *value) {
    if (!buf || !key || !value)
        return SC_ERR_INVALID_ARGUMENT;
    return sc_json_append_key_value(buf, key, strlen(key), value, strlen(value));
}

sc_error_t sc_json_util_append_key_int(sc_json_buf_t *buf, const char *key, int64_t value) {
    if (!buf || !key)
        return SC_ERR_INVALID_ARGUMENT;
    return sc_json_append_key_int(buf, key, strlen(key), (long long)value);
}
