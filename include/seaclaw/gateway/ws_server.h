#ifndef SC_WS_SERVER_H
#define SC_WS_SERVER_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_WS_SERVER_MAX_CONNS    8
#define SC_WS_SERVER_MAX_MSG      (64 * 1024)
#define SC_WS_SERVER_RECV_BUF     (8 * 1024)

typedef struct sc_ws_conn {
    int fd;
    bool active;
    bool authenticated;
    uint64_t id;
    char recv_buf[SC_WS_SERVER_RECV_BUF];
    size_t recv_len;
} sc_ws_conn_t;

typedef void (*sc_ws_server_on_message_fn)(
    sc_ws_conn_t *conn, const char *data, size_t data_len, void *ctx);

typedef void (*sc_ws_server_on_close_fn)(
    sc_ws_conn_t *conn, void *ctx);

typedef struct sc_ws_server {
    sc_allocator_t *alloc;
    sc_ws_conn_t conns[SC_WS_SERVER_MAX_CONNS];
    size_t conn_count;
    uint64_t next_id;
    sc_ws_server_on_message_fn on_message;
    sc_ws_server_on_close_fn on_close;
    void *cb_ctx;
    /* Optional: when set, WebSocket upgrade requires Authorization: Bearer <token> */
    const char *auth_token;
} sc_ws_server_t;

void sc_ws_server_init(sc_ws_server_t *srv, sc_allocator_t *alloc,
    sc_ws_server_on_message_fn on_message,
    sc_ws_server_on_close_fn on_close,
    void *cb_ctx);

void sc_ws_server_deinit(sc_ws_server_t *srv);

/* Attempt WebSocket upgrade on an accepted HTTP connection.
 * req/req_len contain the raw HTTP request already received.
 * On success the fd is added to the connection pool and *out is set.
 * Returns SC_OK on success. */
sc_error_t sc_ws_server_upgrade(sc_ws_server_t *srv, int fd,
    const char *req, size_t req_len,
    sc_ws_conn_t **out);

/* Send a text frame to a single connection. */
sc_error_t sc_ws_server_send(sc_ws_conn_t *conn,
    const char *data, size_t data_len);

/* Broadcast a text frame to all active connections. */
void sc_ws_server_broadcast(sc_ws_server_t *srv,
    const char *data, size_t data_len);

/* Process pending data on a connection. Calls on_message/on_close as needed.
 * Returns SC_OK if connection is still alive, SC_ERR_IO if closed. */
sc_error_t sc_ws_server_process(sc_ws_server_t *srv, sc_ws_conn_t *conn);

/* Read from the connection's fd into its recv_buf, then process.
 * Returns SC_OK if still alive, SC_ERR_IO if closed. */
sc_error_t sc_ws_server_read_and_process(sc_ws_server_t *srv,
    sc_ws_conn_t *conn);

/* Close a single connection gracefully. */
void sc_ws_server_close_conn(sc_ws_server_t *srv, sc_ws_conn_t *conn);

/* Returns true if req contains a valid WebSocket upgrade request. */
bool sc_ws_server_is_upgrade(const char *req, size_t req_len);

#endif /* SC_WS_SERVER_H */
