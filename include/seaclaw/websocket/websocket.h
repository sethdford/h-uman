#ifndef SC_WEBSOCKET_H
#define SC_WEBSOCKET_H

#define SC_WS_OP_CONTINUATION 0
#define SC_WS_OP_TEXT         1
#define SC_WS_OP_BINARY       2
#define SC_WS_OP_CLOSE        8
#define SC_WS_OP_PING         9
#define SC_WS_OP_PONG         10

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

typedef struct sc_ws_client sc_ws_client_t;

sc_error_t sc_ws_connect(sc_allocator_t *alloc,
    const char *url,
    sc_ws_client_t **out);

sc_error_t sc_ws_send(sc_ws_client_t *ws,
    const char *data,
    size_t data_len);

sc_error_t sc_ws_recv(sc_ws_client_t *ws,
    sc_allocator_t *alloc,
    char **data_out,
    size_t *data_len_out);

void sc_ws_close(sc_ws_client_t *ws, sc_allocator_t *alloc);

/* Frame helpers (for testing and custom use) */
size_t sc_ws_build_frame(char *buf, size_t buf_size,
    unsigned opcode,
    const char *payload, size_t payload_len,
    const unsigned char mask_key[4]);

void sc_ws_apply_mask(char *payload, size_t len, const unsigned char mask_key[4]);

typedef struct {
    unsigned opcode;
    int fin;
    int masked;
    unsigned long long payload_len;
    size_t header_bytes;
} sc_ws_parsed_header_t;

int sc_ws_parse_header(const char *bytes, size_t bytes_len, sc_ws_parsed_header_t *out);

#endif
