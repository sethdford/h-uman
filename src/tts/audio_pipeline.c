/* Audio format pipeline: MP3 → CAF for iMessage native voice memo.
 * Requires HU_ENABLE_CARTESIA. Uses afconvert on macOS. */
#if defined(HU_ENABLE_CARTESIA)

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/process_util.h"
#include "human/tts/audio_pipeline.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

hu_error_t hu_audio_mp3_to_caf(hu_allocator_t *alloc,
    const unsigned char *mp3_bytes, size_t mp3_len,
    char *out_audio_path, size_t out_path_cap) {
    (void)alloc;
    if (!mp3_bytes || mp3_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!out_audio_path || out_path_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Test path: write to fixed mock file, skip afconvert. */
    static const char *mock_path = "/tmp/human-voice-test.mp3";
    FILE *f = fopen(mock_path, "wb");
    if (!f)
        return HU_ERR_IO;
    if (fwrite(mp3_bytes, 1, mp3_len, f) != mp3_len) {
        fclose(f);
        return HU_ERR_IO;
    }
    fclose(f);
    int n = snprintf(out_audio_path, out_path_cap, "%s", mock_path);
    if (n < 0 || (size_t)n >= out_path_cap)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;

#elif defined(__APPLE__)
    /* 1. Create temp MP3 via mkstemps */
    char mp3_path[256];
    int n_mp3 = snprintf(mp3_path, sizeof(mp3_path), "/tmp/human-voice-XXXXXX.mp3");
    if (n_mp3 < 0 || (size_t)n_mp3 >= sizeof(mp3_path))
        return HU_ERR_IO;

    int fd = mkstemps(mp3_path, 4);
    if (fd < 0)
        return HU_ERR_IO;

    /* 2. Write mp3_bytes to file */
    ssize_t nw = write(fd, mp3_bytes, mp3_len);
    close(fd);
    if (nw < 0 || (size_t)nw != mp3_len) {
        unlink(mp3_path);
        return HU_ERR_IO;
    }

    /* 3. Build CAF path by replacing .mp3 with .caf */
    char caf_path[256];
    size_t mp3_path_len = strlen(mp3_path);
    if (mp3_path_len < 5 || strcmp(mp3_path + mp3_path_len - 4, ".mp3") != 0) {
        unlink(mp3_path);
        return HU_ERR_INVALID_ARGUMENT;
    }
    int n_caf = snprintf(caf_path, sizeof(caf_path), "%.*s.caf", (int)(mp3_path_len - 4), mp3_path);
    if (n_caf < 0 || (size_t)n_caf >= sizeof(caf_path)) {
        unlink(mp3_path);
        return HU_ERR_IO;
    }

    /* 4. Run afconvert: -f caff -d aac -b 128000 input.mp3 -o output.caf */
    const char *argv[] = {
        "afconvert", "-f", "caff", "-d", "aac", "-b", "128000",
        mp3_path, "-o", caf_path, NULL
    };
    hu_run_result_t result = {0};
    hu_error_t run_err = hu_process_run(alloc, argv, NULL, 4096, &result);
    hu_run_result_free(alloc, &result);

    if (run_err == HU_OK && result.success) {
        /* 5. Success: copy caf_path to out, unlink mp3 */
        unlink(mp3_path);
        int n_out = snprintf(out_audio_path, out_path_cap, "%s", caf_path);
        if (n_out < 0 || (size_t)n_out >= out_path_cap)
            return HU_ERR_INVALID_ARGUMENT;
        return HU_OK;
    }

    /* 6. Fallback: copy mp3_path to out (iMessage accepts MP3 too) */
    unlink(caf_path);
    int n_out = snprintf(out_audio_path, out_path_cap, "%s", mp3_path);
    if (n_out < 0 || (size_t)n_out >= out_path_cap)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;

#else
    (void)mp3_bytes;
    (void)mp3_len;
    (void)out_audio_path;
    (void)out_path_cap;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

void hu_audio_cleanup_temp(const char *audio_path) {
    if (audio_path && audio_path[0])
        (void)unlink(audio_path);
}

hu_error_t hu_audio_pipeline_process(hu_allocator_t *alloc, const void *input,
    size_t input_len, void **out, size_t *out_len) {
    (void)alloc;
    (void)input;
    (void)input_len;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

#else /* !HU_ENABLE_CARTESIA */

#include "human/core/error.h"
#include "human/tts/audio_pipeline.h"
#include <stddef.h>

hu_error_t hu_audio_mp3_to_caf(hu_allocator_t *alloc,
    const unsigned char *mp3_bytes, size_t mp3_len,
    char *out_audio_path, size_t out_path_cap) {
    (void)alloc;
    (void)mp3_bytes;
    (void)mp3_len;
    (void)out_audio_path;
    (void)out_path_cap;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_audio_cleanup_temp(const char *audio_path) {
    (void)audio_path;
}

hu_error_t hu_audio_pipeline_process(hu_allocator_t *alloc, const void *input,
    size_t input_len, void **out, size_t *out_len) {
    (void)alloc;
    (void)input;
    (void)input_len;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_CARTESIA */
