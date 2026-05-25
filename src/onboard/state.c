#include "human/onboard/state.h"
#include "human/core/error.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Magic number and version for the onboard state file format. */
#define HU_ONBOARD_STATE_MAGIC   0x48554F42 /* "HUOB" in hex */
#define HU_ONBOARD_STATE_VERSION 1

typedef struct {
    uint32_t magic;
    int version;
} hu_onboard_state_header_t;

/**
 * Ensure the parent directory of path exists.
 * Returns HU_OK on success, HU_ERR_IO if mkdir fails.
 */
static hu_error_t ensure_parent_dir(const char *path) {
    if (!path || !*path)
        return HU_ERR_INVALID_ARGUMENT;

    char dir[512];
    size_t len = strlen(path);
    if (len >= sizeof(dir) - 1)
        return HU_ERR_INVALID_ARGUMENT;

    strcpy(dir, path);
    char *sep = strrchr(dir, '/');
    if (!sep)
        return HU_OK; /* no directory component */

    *sep = '\0';
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return HU_ERR_IO;

    return HU_OK;
}

void hu_onboard_state_init(hu_onboard_state_t *state) {
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->schema_version = HU_ONBOARD_STATE_VERSION;
    state->current = HU_ONBOARD_STEP_WELCOME;
    state->history_depth = 0;
}

hu_error_t hu_onboard_state_save(const hu_onboard_state_t *state, const char *path) {
    if (!state || !path || !*path)
        return HU_ERR_INVALID_ARGUMENT;

    if (ensure_parent_dir(path) != HU_OK)
        return HU_ERR_IO;

    /* Atomic write via tmp + fsync + rename.
     * Crash safety:
     *   - Crash before fclose: <path>.tmp is partial, <path> is untouched,
     *     load returns the prior state.
     *   - Crash after rename: <path> is the new file, intact.
     *   - No in-between window: rename(2) is atomic on POSIX. */

    char tmp[512];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(tmp, "wb");
    if (!fp)
        return HU_ERR_IO;

    /* Write header */
    hu_onboard_state_header_t hdr;
    hdr.magic = HU_ONBOARD_STATE_MAGIC;
    hdr.version = HU_ONBOARD_STATE_VERSION;

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }

    /* Write state */
    if (fwrite(state, sizeof(*state), 1, fp) != 1) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }

    /* fflush drains stdio buffers; fsync forces the kernel page cache
     * to disk so a power loss between rename and writeback can't leave
     * the renamed file with stale or zero contents. */
    if (fflush(fp) != 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }

    int fd = fileno(fp);
    if (fd >= 0 && fsync(fd) != 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }

    if (fclose(fp) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }

    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }

    return HU_OK;
}

hu_error_t hu_onboard_state_load(hu_onboard_state_t *out, const char *path) {
    if (!out || !path || !*path)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HU_ERR_IO;

    hu_onboard_state_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        return HU_ERR_IO;
    }

    /* Validate magic and version */
    if (hdr.magic != HU_ONBOARD_STATE_MAGIC) {
        fclose(fp);
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (hdr.version != HU_ONBOARD_STATE_VERSION) {
        fclose(fp);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_onboard_state_t temp;
    if (fread(&temp, sizeof(temp), 1, fp) != 1) {
        fclose(fp);
        return HU_ERR_IO;
    }

    fclose(fp);

    /* Validate the loaded state's schema version as a sanity check */
    if (temp.schema_version != HU_ONBOARD_STATE_VERSION)
        return HU_ERR_INVALID_ARGUMENT;

    /* Copy to output only after all validation passes */
    memcpy(out, &temp, sizeof(*out));
    return HU_OK;
}
