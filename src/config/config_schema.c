#include "config_internal.h"
#include <string.h>

hu_sandbox_backend_t hu_config_parse_sandbox_backend(const char *s) {
    if (!s)
        return HU_SANDBOX_AUTO;
    if (strcmp(s, "landlock") == 0)
        return HU_SANDBOX_LANDLOCK;
    if (strcmp(s, "firejail") == 0)
        return HU_SANDBOX_FIREJAIL;
    if (strcmp(s, "bubblewrap") == 0)
        return HU_SANDBOX_BUBBLEWRAP;
    if (strcmp(s, "docker") == 0)
        return HU_SANDBOX_DOCKER;
    if (strcmp(s, "seatbelt") == 0)
        return HU_SANDBOX_SEATBELT;
    if (strcmp(s, "seccomp") == 0)
        return HU_SANDBOX_SECCOMP;
    if (strcmp(s, "landlock+seccomp") == 0)
        return HU_SANDBOX_LANDLOCK_SECCOMP;
    if (strcmp(s, "wasi") == 0)
        return HU_SANDBOX_WASI;
    if (strcmp(s, "firecracker") == 0)
        return HU_SANDBOX_FIRECRACKER;
    if (strcmp(s, "appcontainer") == 0)
        return HU_SANDBOX_APPCONTAINER;
    if (strcmp(s, "none") == 0)
        return HU_SANDBOX_NONE;
    return HU_SANDBOX_AUTO;
}
