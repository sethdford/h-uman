#ifndef HU_WS_SERVER_H
#define HU_WS_SERVER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_WS_SERVER_MAX_CONNS 32
#define HU_WS_SERVER_MAX_MSG   (512 * 1024)
#define HU_WS_SERVER_RECV_BUF  (8 * 1024)
#define HU_WS_SERVER_RECV_MAX  (512 * 1024)

typedef enum hu_rpc_auth_level {
    HU_RPC_AUTH_NONE = 0,
    HU_RPC_AUTH_PAIRED = 1,
} hu_rpc_auth_level_t;

typedef struct hu_ws_conn {
    int fd;
    bool active;
    bool authenticated;
    uint64_t id;
    char inline_buf[HU_WS_SERVER_RECV_BUF];
    char *recv_buf;
    size_t recv_len;
    size_t recv_cap;
} hu_ws_conn_t;

typedef void (*hu_ws_server_on_message_fn)(hu_ws_conn_t *conn, const char *data, size_t data_len,
                                           void *ctx);

typedef void (*hu_ws_server_on_close_fn)(hu_ws_conn_t *conn, void *ctx);

typedef struct hu_ws_server {
    hu_allocator_t *alloc;
    hu_ws_conn_t conns[HU_WS_SERVER_MAX_CONNS];
    size_t conn_count;
    uint64_t next_id;
    hu_ws_server_on_message_fn on_message;
    hu_ws_server_on_close_fn on_close;
    void *cb_ctx;
    /* Optional: when set, WebSocket upgrade requires Authorization: Bearer <token> */
    const char *auth_token;
} hu_ws_server_t;

void hu_ws_server_init(hu_ws_server_t *srv, hu_allocator_t *alloc,
                       hu_ws_server_on_message_fn on_message, hu_ws_server_on_close_fn on_close,
                       void *cb_ctx);

void hu_ws_server_deinit(hu_ws_server_t *srv);

/* Attempt WebSocket upgrade on an accepted HTTP connection.
 * req/req_len contain the raw HTTP request already received.
 * On success the fd is added to the connection pool and *out is set.
 * Returns HU_OK on success. */
hu_error_t hu_ws_server_upgrade(hu_ws_server_t *srv, int fd, const char *req, size_t req_len,
                                hu_ws_conn_t **out);

/* Send a text frame to a single connection. */
hu_error_t hu_ws_server_send(hu_ws_conn_t *conn, const char *data, size_t data_len);

/* Broadcast a text frame to all active connections. */
void hu_ws_server_broadcast(hu_ws_server_t *srv, const char *data, size_t data_len);

/* Process pending data on a connection. Calls on_message/on_close as needed.
 * Returns HU_OK if connection is still alive, HU_ERR_IO if closed. */
hu_error_t hu_ws_server_process(hu_ws_server_t *srv, hu_ws_conn_t *conn);

/* Read from the connection's fd into its recv_buf, then process.
 * Returns HU_OK if still alive, HU_ERR_IO if closed. */
hu_error_t hu_ws_server_read_and_process(hu_ws_server_t *srv, hu_ws_conn_t *conn);

/* Close a single connection gracefully. */
void hu_ws_server_close_conn(hu_ws_server_t *srv, hu_ws_conn_t *conn);

/* Returns true if req contains a valid WebSocket upgrade request. */
bool hu_ws_server_is_upgrade(const char *req, size_t req_len);

#endif /* HU_WS_SERVER_H */
