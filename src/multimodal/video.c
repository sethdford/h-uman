#include "human/multimodal/video.h"
#include "human/auth.h"
#include "human/context/vision.h"
#include "human/core/json.h"
#include "human/core/privacy.h"
#include "human/core/string.h"
#include "human/multimodal.h"
#include "human/voice.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (!defined(HU_IS_TEST) || !HU_IS_TEST) && !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>

static void hu_video_cleanup_tmpdir(const char *dir) {
    if (!dir || !dir[0])
        return;
    for (int fi = 1; fi <= 5; fi++) {
        char fp[512];
        int n = snprintf(fp, sizeof(fp), "%s/frame_%03d.jpg", dir, fi);
        if (n > 0 && (size_t)n < sizeof(fp))
            (void)remove(fp);
    }
    (void)rmdir(dir);
}

static hu_error_t hu_video_run_ffmpeg_extract(const char *input_path, const char *tmpdir) {
    char out_pat[512];
    int pn = snprintf(out_pat, sizeof(out_pat), "%s/frame_%%03d.jpg", tmpdir);
    if (pn <= 0 || (size_t)pn >= sizeof(out_pat))
        return HU_ERR_IO;

    pid_t pid = fork();
    if (pid < 0)
        return HU_ERR_IO;
    if (pid == 0) {
        char vf[] = "fps=1/3";
        char fr[] = "5";
        char qv[] = "2";
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i",
               input_path, "-vf", vf, "-frames:v", fr, "-q:v", qv, out_pat, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return HU_ERR_IO;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return HU_ERR_IO;
    return HU_OK;
}
#endif

static int to_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool match_ext(const char *filename, size_t filename_len, const char *ext, size_t ext_len) {
    if (filename_len < ext_len + 1)
        return false;
    if (filename[filename_len - ext_len - 1] != '.')
        return false;
    for (size_t i = 0; i < ext_len; i++) {
        if (to_lower((unsigned char)filename[filename_len - ext_len + i]) != (unsigned char)ext[i])
            return false;
    }
    return true;
}

static bool is_supported_video_ext(const char *path, size_t path_len) {
    return match_ext(path, path_len, "mp4", 3) || match_ext(path, path_len, "mov", 3) ||
           match_ext(path, path_len, "webm", 4) || match_ext(path, path_len, "avi", 3);
}

hu_error_t hu_multimodal_process_video(hu_allocator_t *alloc, const char *file_path,
                                       size_t path_len, hu_provider_t *provider, const char *model,
                                       size_t model_len, char **out_text, size_t *out_text_len) {
    if (!alloc || !file_path || path_len == 0 || !provider || !out_text || !out_text_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!provider->vtable || !provider->vtable->get_name)
        return HU_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_text_len = 0;

    if (!is_supported_video_ext(file_path, path_len))
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)model;
    (void)model_len;
    (void)provider;
    const char *mock = "Mock video description: person walking in park";
    size_t mlen = strlen(mock);
    char *dup = hu_strndup(alloc, mock, mlen);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out_text = dup;
    *out_text_len = mlen;
    return HU_OK;
#elif defined(_WIN32)
    (void)file_path;
    (void)provider;
    (void)model;
    (void)model_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    char path_buf[4096];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, file_path, path_len);
    path_buf[path_len] = '\0';

    const char *vision_model = "gpt-4o";
    char model_buf[160];
    if (model && model_len > 0) {
        if (model_len >= sizeof(model_buf))
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(model_buf, model, model_len);
        model_buf[model_len] = '\0';
        vision_model = model_buf;
    }
    size_t vision_model_len = strlen(vision_model);

    /* Gemini natively processes video — send bytes via generateContent (see hu_voice_stt_gemini).
     */
    {
        const char *pname = provider->vtable->get_name(provider->ctx);
        if (pname && (strstr(pname, "gemini") != NULL || strstr(pname, "google") != NULL)) {
            FILE *vf = fopen(path_buf, "rb");
            if (!vf)
                goto ffmpeg_fallback;
            if (fseek(vf, 0, SEEK_END) != 0) {
                fclose(vf);
                goto ffmpeg_fallback;
            }
            long vsz = ftell(vf);
            if (vsz <= 0 || vsz > (long)(50U * 1024U * 1024U)) {
                fclose(vf);
                goto ffmpeg_fallback;
            }
            rewind(vf);
            char *vbuf = (char *)alloc->alloc(alloc->ctx, (size_t)vsz);
            if (!vbuf) {
                fclose(vf);
                goto ffmpeg_fallback;
            }
            size_t vread = fread(vbuf, 1, (size_t)vsz, vf);
            fclose(vf);
            if (vread != (size_t)vsz) {
                alloc->free(alloc->ctx, vbuf, (size_t)vsz);
                goto ffmpeg_fallback;
            }

            char *b64 = NULL;
            size_t b64_len = 0;
            hu_error_t enc_err =
                hu_multimodal_encode_base64(alloc, vbuf, (size_t)vsz, &b64, &b64_len);
            alloc->free(alloc->ctx, vbuf, (size_t)vsz);
            if (enc_err != HU_OK || !b64)
                goto ffmpeg_fallback;

            const char *mime = "video/mp4";
            if (match_ext(file_path, path_len, "webm", 4))
                mime = "video/webm";
            else if (match_ext(file_path, path_len, "mov", 3))
                mime = "video/quicktime";

            char *api_key = hu_auth_get_api_key(alloc, "gemini");
            if (!api_key || api_key[0] == '\0') {
                if (api_key)
                    alloc->free(alloc->ctx, api_key, strlen(api_key) + 1);
                alloc->free(alloc->ctx, b64, b64_len + 1);
                goto ffmpeg_fallback;
            }
            size_t api_key_len = strlen(api_key);
            char *model_owned = NULL;
            const char *model_use = NULL;
            if (model && model_len > 0) {
                model_owned = hu_strndup(alloc, model, model_len);
                if (!model_owned) {
                    alloc->free(alloc->ctx, api_key, api_key_len + 1);
                    alloc->free(alloc->ctx, b64, b64_len + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                model_use = model_owned;
            }
            hu_voice_config_t vcfg = {.api_key = api_key,
                                      .api_key_len = api_key_len,
                                      .stt_model = model_use,
                                      .stt_endpoint = NULL,
                                      .tts_endpoint = NULL,
                                      .tts_model = NULL,
                                      .tts_voice = NULL,
                                      .language = NULL};
            char *desc = NULL;
            size_t desc_len = 0;
            hu_error_t gerr =
                hu_voice_stt_gemini(alloc, &vcfg, b64, b64_len, mime, &desc, &desc_len);
            alloc->free(alloc->ctx, b64, b64_len + 1);
            alloc->free(alloc->ctx, api_key, api_key_len + 1);
            if (model_owned)
                alloc->free(alloc->ctx, model_owned, model_len + 1);
            if (gerr == HU_OK && desc && desc_len > 0) {
                *out_text = desc;
                *out_text_len = desc_len;
                return HU_OK;
            }
            if (desc)
                alloc->free(alloc->ctx, desc, desc_len + 1);
        }
    }

ffmpeg_fallback: {
    /* Privacy kill-switch: don't extract frames or send them to a cloud vision
     * provider. (hu_vision_describe_image also enforces this, but stop earlier here so
     * we skip frame extraction entirely and return a clean placeholder.) */
    if (hu_privacy_enforced()) {
        const char *blocked = "[Video received — not described: privacy mode (on-device only)]";
        size_t blen = strlen(blocked);
        char *dup = hu_strndup(alloc, blocked, blen);
        if (!dup)
            return HU_ERR_OUT_OF_MEMORY;
        *out_text = dup;
        *out_text_len = blen;
        return HU_OK;
    }

    char tmpl[] = "/tmp/hu_video_XXXXXX";
    if (!mkdtemp(tmpl))
        return HU_ERR_IO;

    hu_error_t ff_err = hu_video_run_ffmpeg_extract(path_buf, tmpl);

    if (ff_err != HU_OK) {
        hu_video_cleanup_tmpdir(tmpl);
        const char *fallback = "[Video received — ffmpeg not available for frame extraction]";
        size_t flen = strlen(fallback);
        char *dup = hu_strndup(alloc, fallback, flen);
        if (!dup)
            return HU_ERR_OUT_OF_MEMORY;
        *out_text = dup;
        *out_text_len = flen;
        return HU_OK;
    }

    hu_json_buf_t desc_buf;
    hu_error_t berr = hu_json_buf_init(&desc_buf, alloc);
    if (berr != HU_OK) {
        hu_video_cleanup_tmpdir(tmpl);
        return berr;
    }

    const char *vid_header = "[Video description]\n";
    berr = hu_json_buf_append_raw(&desc_buf, vid_header, strlen(vid_header));
    if (berr != HU_OK) {
        hu_json_buf_free(&desc_buf);
        hu_video_cleanup_tmpdir(tmpl);
        return berr;
    }

    bool got_desc = false;
    for (int fi = 1; fi <= 5; fi++) {
        char frame_path[600];
        int fn = snprintf(frame_path, sizeof(frame_path), "%s/frame_%03d.jpg", tmpl, fi);
        if (fn <= 0 || (size_t)fn >= sizeof(frame_path))
            continue;

        FILE *fp = fopen(frame_path, "rb");
        if (!fp)
            continue;
        fclose(fp);

        if (provider->vtable && provider->vtable->supports_vision &&
            provider->vtable->supports_vision(provider->ctx)) {
            char *frame_desc = NULL;
            size_t frame_desc_len = 0;
            hu_error_t verr =
                hu_vision_describe_image(alloc, provider, frame_path, (size_t)fn, vision_model,
                                         vision_model_len, &frame_desc, &frame_desc_len);
            if (verr == HU_OK && frame_desc && frame_desc_len > 0) {
                got_desc = true;
                char ts_prefix[64];
                int tp = snprintf(ts_prefix, sizeof(ts_prefix), "~%ds: ", (fi - 1) * 3);
                if (tp > 0 && (size_t)tp < sizeof(ts_prefix)) {
                    berr = hu_json_buf_append_raw(&desc_buf, ts_prefix, (size_t)tp);
                    if (berr != HU_OK) {
                        alloc->free(alloc->ctx, frame_desc, frame_desc_len + 1);
                        hu_json_buf_free(&desc_buf);
                        hu_video_cleanup_tmpdir(tmpl);
                        return berr;
                    }
                }
                berr = hu_json_buf_append_raw(&desc_buf, frame_desc, frame_desc_len);
                if (berr != HU_OK) {
                    alloc->free(alloc->ctx, frame_desc, frame_desc_len + 1);
                    hu_json_buf_free(&desc_buf);
                    hu_video_cleanup_tmpdir(tmpl);
                    return berr;
                }
                berr = hu_json_buf_append_raw(&desc_buf, "\n", 1);
                if (berr != HU_OK) {
                    alloc->free(alloc->ctx, frame_desc, frame_desc_len + 1);
                    hu_json_buf_free(&desc_buf);
                    hu_video_cleanup_tmpdir(tmpl);
                    return berr;
                }
                alloc->free(alloc->ctx, frame_desc, frame_desc_len + 1);
            }
        }
    }

    hu_video_cleanup_tmpdir(tmpl);

    berr = hu_json_buf_append_raw(&desc_buf, "\0", 1);
    if (berr != HU_OK) {
        hu_json_buf_free(&desc_buf);
        return berr;
    }

    if (!got_desc) {
        hu_json_buf_free(&desc_buf);
        const char *empty = "[Video received — no frames could be described]";
        size_t elen = strlen(empty);
        char *dup = hu_strndup(alloc, empty, elen);
        if (!dup)
            return HU_ERR_OUT_OF_MEMORY;
        *out_text = dup;
        *out_text_len = elen;
        return HU_OK;
    }

    *out_text = desc_buf.ptr;
    *out_text_len = desc_buf.len > 0 ? desc_buf.len - 1 : 0;
    desc_buf.ptr = NULL;
    hu_json_buf_free(&desc_buf);
    return HU_OK;
}
#endif
}
