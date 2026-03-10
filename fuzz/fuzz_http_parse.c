/* libFuzzer harness for HTTP request parsing in the gateway.
 * Exercises hu_ws_server_is_upgrade which parses Upgrade/Connection headers
 * via extract_header — the same path used for WebSocket upgrade detection
 * and HTTP header extraction in gateway.c. */
#include "human/gateway/ws_server.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HU_FUZZ_HTTP_MAX_INPUT 16384

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > HU_FUZZ_HTTP_MAX_INPUT)
        return 0;

    char *buf = malloc(size + 1);
    if (!buf)
        return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    (void)hu_ws_server_is_upgrade(buf, size);

    free(buf);
    return 0;
}
