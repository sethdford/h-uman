#include "config_internal.h"
#include <string.h>

const char *sc_config_sandbox_backend_to_string(sc_sandbox_backend_t b) {
    switch (b) {
    case SC_SANDBOX_AUTO:
        return "auto";
    case SC_SANDBOX_NONE:
        return "none";
    case SC_SANDBOX_LANDLOCK:
        return "landlock";
    case SC_SANDBOX_FIREJAIL:
        return "firejail";
    case SC_SANDBOX_BUBBLEWRAP:
        return "bubblewrap";
    case SC_SANDBOX_DOCKER:
        return "docker";
    case SC_SANDBOX_SEATBELT:
        return "seatbelt";
    case SC_SANDBOX_SECCOMP:
        return "seccomp";
    case SC_SANDBOX_LANDLOCK_SECCOMP:
        return "landlock_seccomp";
    case SC_SANDBOX_WASI:
        return "wasi";
    case SC_SANDBOX_FIRECRACKER:
        return "firecracker";
    case SC_SANDBOX_APPCONTAINER:
        return "appcontainer";
    }
    return "auto";
}

sc_sandbox_backend_t sc_config_parse_sandbox_backend(const char *s) {
    if (!s)
        return SC_SANDBOX_AUTO;
    if (strcmp(s, "landlock") == 0)
        return SC_SANDBOX_LANDLOCK;
    if (strcmp(s, "firejail") == 0)
        return SC_SANDBOX_FIREJAIL;
    if (strcmp(s, "bubblewrap") == 0)
        return SC_SANDBOX_BUBBLEWRAP;
    if (strcmp(s, "docker") == 0)
        return SC_SANDBOX_DOCKER;
    if (strcmp(s, "seatbelt") == 0)
        return SC_SANDBOX_SEATBELT;
    if (strcmp(s, "seccomp") == 0)
        return SC_SANDBOX_SECCOMP;
    if (strcmp(s, "landlock+seccomp") == 0)
        return SC_SANDBOX_LANDLOCK_SECCOMP;
    if (strcmp(s, "wasi") == 0)
        return SC_SANDBOX_WASI;
    if (strcmp(s, "firecracker") == 0)
        return SC_SANDBOX_FIRECRACKER;
    if (strcmp(s, "appcontainer") == 0)
        return SC_SANDBOX_APPCONTAINER;
    if (strcmp(s, "none") == 0)
        return SC_SANDBOX_NONE;
    return SC_SANDBOX_AUTO;
}
