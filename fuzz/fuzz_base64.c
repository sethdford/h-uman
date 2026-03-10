/* libFuzzer harness for base64url_decode (Gmail body). Must not crash on any input. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* Declared in src/channels/gmail_base64.c */
hu_error_t base64url_decode(const char *in, size_t in_len, char *out, size_t out_cap,
                            size_t *out_len);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char out[4096];
    size_t out_len = 0;
    base64url_decode((const char *)data, size, out, sizeof(out), &out_len);
    return 0;
}
