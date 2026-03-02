#ifndef SC_HOST_IMPORTS_H
#define SC_HOST_IMPORTS_H

#ifdef __wasi__

/* Functions that must be provided by the WASM host runtime.
 * These use __attribute__((import_module("seaclaw"), import_name("..."))).
 * The host (e.g. Node, Wasmtime, or a custom runtime) must implement these. */

#ifdef __clang__
#define SC_IMPORT(mod, name) __attribute__((import_module(mod), import_name(name)))
#else
#define SC_IMPORT(mod, name)
#endif

/* HTTP fetch: perform HTTP request.
 * url, method, body may be NULL if len is 0.
 * Returns byte count written to response_buf, or negative on error. */
SC_IMPORT("seaclaw", "http_fetch")
int sc_host_http_fetch(const char *url, int url_len, const char *method, int method_len,
    const char *body, int body_len, char *response_buf, int buf_len);

/* Log message to host. level: 0=debug, 1=info, 2=warn, 3=error */
SC_IMPORT("seaclaw", "log")
void sc_host_log(const char *msg, int msg_len, int level);

/* Optional: get HOME directory path for config discovery.
 * Writes to buf, returns length. 0 if not available. */
SC_IMPORT("seaclaw", "get_home")
int sc_host_get_home(char *buf, int buf_len);

#endif /* __wasi__ */

#endif /* SC_HOST_IMPORTS_H */
