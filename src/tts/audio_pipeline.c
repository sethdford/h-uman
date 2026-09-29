/* Audio format pipeline: MP3 → CAF for iMessage native voice memo.
 * Requires HU_ENABLE_CARTESIA. Uses afconvert on macOS. */
#if defined(HU_ENABLE_CARTESIA)

#include "human/tts/audio_pipeline.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/process_util.h"
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

hu_error_t hu_audio_mp3_to_caf(hu_allocator_t *alloc, const unsigned char *mp3_bytes,
                               size_t mp3_len, char *out_audio_path, size_t out_path_cap) {
    (void)alloc;
    if (!mp3_bytes || mp3_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!out_audio_path || out_path_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Test path: write a per-process mock file, skip afconvert. The pid keeps
     * two concurrently running suites from writing/unlinking the same file
     * underneath each other's assertions. */
    char mock_path[256];
    int nm =
        snprintf(mock_path, sizeof(mock_path), "/tmp/human-voice-test-%ld.mp3", (long)getpid());
    if (nm < 0 || (size_t)nm >= sizeof(mock_path))
        return HU_ERR_IO;
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
    const char *argv[] = {"afconvert", "-f",     "caff", "-d",     "aac", "-b",
                          "128000",    mp3_path, "-o",   caf_path, NULL};
    hu_run_result_t result = {0};
    hu_error_t run_err = hu_process_run(alloc, argv, NULL, 4096, &result);
    bool ok = (run_err == HU_OK && result.success);
    hu_run_result_free(alloc, &result);

    if (ok) {
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

hu_error_t hu_audio_tts_bytes_to_temp(hu_allocator_t *alloc, const unsigned char *bytes,
                                      size_t bytes_len, const char *file_ext_no_dot, char *out_path,
                                      size_t out_cap) {
    (void)alloc;
    if (!bytes || bytes_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!out_path || out_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!file_ext_no_dot || !file_ext_no_dot[0])
        return HU_ERR_INVALID_ARGUMENT;
    if (strcmp(file_ext_no_dot, "mp3") != 0 && strcmp(file_ext_no_dot, "wav") != 0)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Per-process mock path — see hu_audio_mp3_to_caf. */
    int n = snprintf(out_path, out_cap, "/tmp/human-tts-bytes-test-%ld.%s", (long)getpid(),
                     file_ext_no_dot);
    if (n < 0 || (size_t)n >= out_cap)
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(out_path, "wb");
    if (!f)
        return HU_ERR_IO;
    if (fwrite(bytes, 1, bytes_len, f) != bytes_len) {
        fclose(f);
        return HU_ERR_IO;
    }
    fclose(f);
    return HU_OK;
#else
    char path_template[256];
    size_t elen = strlen(file_ext_no_dot);
    if (elen > 16)
        return HU_ERR_INVALID_ARGUMENT;
    int nt =
        snprintf(path_template, sizeof(path_template), "/tmp/human-tts-XXXXXX.%s", file_ext_no_dot);
    if (nt < 0 || (size_t)nt >= sizeof(path_template))
        return HU_ERR_IO;

    int suffix_len = (int)(1 + elen); /* .ext */
    int fd = mkstemps(path_template, suffix_len);
    if (fd < 0)
        return HU_ERR_IO;
    ssize_t nw = write(fd, bytes, bytes_len);
    close(fd);
    if (nw < 0 || (size_t)nw != bytes_len) {
        unlink(path_template);
        return HU_ERR_IO;
    }
    int no = snprintf(out_path, out_cap, "%s", path_template);
    if (no < 0 || (size_t)no >= out_cap) {
        unlink(path_template);
        return HU_ERR_INVALID_ARGUMENT;
    }
    return HU_OK;
#endif
}

typedef enum {
    AUDIO_FMT_UNKNOWN,
    AUDIO_FMT_MP3,
    AUDIO_FMT_WAV,
    AUDIO_FMT_OGG,
    AUDIO_FMT_PCM,
} audio_format_t;

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static audio_format_t detect_audio_format(const unsigned char *data, size_t len) {
    if (!data || len < 4)
        return AUDIO_FMT_PCM;
    if (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0)
        return AUDIO_FMT_MP3;
    if (len >= 3 && data[0] == 'I' && data[1] == 'D' && data[2] == '3')
        return AUDIO_FMT_MP3;
    if (len >= 4 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F')
        return AUDIO_FMT_WAV;
    if (len >= 4 && data[0] == 'O' && data[1] == 'g' && data[2] == 'g' && data[3] == 'S')
        return AUDIO_FMT_OGG;
    return AUDIO_FMT_PCM;
}
#endif

static hu_error_t pass_through(hu_allocator_t *alloc, const void *input, size_t input_len,
                               void **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (input_len == 0) {
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }
    if (!input)
        return HU_ERR_INVALID_ARGUMENT;
    void *buf = alloc->alloc(alloc->ctx, input_len);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, input, input_len);
    *out = buf;
    *out_len = input_len;
    return HU_OK;
}

hu_error_t hu_audio_pipeline_process(hu_allocator_t *alloc, const void *input, size_t input_len,
                                     void **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!input && input_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    return pass_through(alloc, input, input_len, out, out_len);
#elif defined(__APPLE__) || defined(__linux__)
    audio_format_t fmt = detect_audio_format((const unsigned char *)input, input_len);
    if (fmt == AUDIO_FMT_PCM || fmt == AUDIO_FMT_UNKNOWN)
        return pass_through(alloc, input, input_len, out, out_len);

    const char *ext;
    switch (fmt) {
    case AUDIO_FMT_MP3:
        ext = ".mp3";
        break;
    case AUDIO_FMT_WAV:
        ext = ".wav";
        break;
    case AUDIO_FMT_OGG:
        ext = ".ogg";
        break;
    default:
        return pass_through(alloc, input, input_len, out, out_len);
    }
    size_t ext_len = strlen(ext);

    char in_path[256];
    int n_in = snprintf(in_path, sizeof(in_path), "/tmp/human-audio-XXXXXX%s", ext);
    if (n_in < 0 || (size_t)n_in >= sizeof(in_path))
        return HU_ERR_IO;

    int fd = mkstemps(in_path, (int)ext_len);
    if (fd < 0)
        return HU_ERR_IO;

    ssize_t nw = write(fd, input, input_len);
    close(fd);
    if (nw < 0 || (size_t)nw != input_len) {
        unlink(in_path);
        return HU_ERR_IO;
    }

    char out_path[256];
    size_t in_path_len = strlen(in_path);
    int n_out_path =
        snprintf(out_path, sizeof(out_path), "%.*s.wav", (int)(in_path_len - ext_len), in_path);
    if (n_out_path < 0 || (size_t)n_out_path >= sizeof(out_path)) {
        unlink(in_path);
        return HU_ERR_IO;
    }

    bool ok = false;
#if defined(__APPLE__)
    const char *argv[] = {"afconvert", "-f",    "WAVE", "-d",     "LEI16@16000", "-c",
                          "1",         in_path, "-o",   out_path, NULL};
    hu_run_result_t result = {0};
    hu_error_t run_err = hu_process_run(alloc, argv, NULL, 4096, &result);
    ok = (run_err == HU_OK && result.success);
    hu_run_result_free(alloc, &result);
#elif defined(__linux__)
    const char *argv[] = {"ffmpeg", "-i",    in_path, "-f", "wav", "-acodec", "pcm_s16le",
                          "-ar",    "16000", "-ac",   "1",  "-y",  out_path,  NULL};
    hu_run_result_t result = {0};
    hu_error_t run_err = hu_process_run(alloc, argv, NULL, 4096, &result);
    ok = (run_err == HU_OK && result.success);
    hu_run_result_free(alloc, &result);
#endif

    unlink(in_path);
    if (!ok) {
        unlink(out_path);
        return pass_through(alloc, input, input_len, out, out_len);
    }

    int out_fd = open(out_path, O_RDONLY);
    if (out_fd < 0) {
        unlink(out_path);
        return pass_through(alloc, input, input_len, out, out_len);
    }

    struct stat st;
    if (fstat(out_fd, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > 64 * 1024 * 1024) {
        close(out_fd);
        unlink(out_path);
        return pass_through(alloc, input, input_len, out, out_len);
    }
    size_t out_size = (size_t)st.st_size;

    void *buf = alloc->alloc(alloc->ctx, out_size);
    if (!buf) {
        close(out_fd);
        unlink(out_path);
        return HU_ERR_OUT_OF_MEMORY;
    }

    ssize_t nr = read(out_fd, buf, out_size);
    close(out_fd);
    unlink(out_path);
    if (nr < 0 || (size_t)nr != out_size) {
        alloc->free(alloc->ctx, buf, out_size);
        return pass_through(alloc, input, input_len, out, out_len);
    }

    *out = buf;
    *out_len = out_size;
    return HU_OK;

#else
    return pass_through(alloc, input, input_len, out, out_len);
#endif
}

#else /* !HU_ENABLE_CARTESIA */

#include "human/core/error.h"
#include "human/tts/audio_pipeline.h"
#include <stddef.h>

hu_error_t hu_audio_mp3_to_caf(hu_allocator_t *alloc, const unsigned char *mp3_bytes,
                               size_t mp3_len, char *out_audio_path, size_t out_path_cap) {
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

hu_error_t hu_audio_tts_bytes_to_temp(hu_allocator_t *alloc, const unsigned char *bytes,
                                      size_t bytes_len, const char *file_ext_no_dot, char *out_path,
                                      size_t out_cap) {
    (void)alloc;
    (void)bytes;
    (void)bytes_len;
    (void)file_ext_no_dot;
    (void)out_path;
    (void)out_cap;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_audio_pipeline_process(hu_allocator_t *alloc, const void *input, size_t input_len,
                                     void **out, size_t *out_len) {
    (void)alloc;
    (void)input;
    (void)input_len;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_CARTESIA */
