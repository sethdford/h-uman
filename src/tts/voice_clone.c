#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include "human/tts/voice_clone.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/file.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/persona.h"
#include "human/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CARTESIA_CLONE_URL "https://api.cartesia.ai/voices/clone"
#define CARTESIA_VERSION   "2026-03-01"

void hu_voice_clone_config_default(hu_voice_clone_config_t *cfg) {
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->language = "en";
    cfg->language_len = 2;
}

#if defined(HU_ENABLE_CARTESIA)

#if defined(HU_HTTP_CURL) && !HU_IS_TEST
static hu_error_t parse_clone_response(hu_allocator_t *alloc, const char *json, size_t json_len,
                                       hu_voice_clone_result_t *out) {
    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &parsed);
    if (err != HU_OK)
        return HU_ERR_PARSE;

    hu_json_value_t *id_val = hu_json_object_get(parsed, "id");
    if (!id_val || id_val->type != HU_JSON_STRING) {
        /* Check for error response */
        hu_json_value_t *err_val = hu_json_object_get(parsed, "error");
        if (err_val && err_val->type == HU_JSON_STRING) {
            size_t elen = err_val->data.string.len < sizeof(out->error) - 1
                              ? err_val->data.string.len
                              : sizeof(out->error) - 1;
            memcpy(out->error, err_val->data.string.ptr, elen);
            out->error[elen] = '\0';
            out->error_len = elen;
        }
        hu_json_free(alloc, parsed);
        out->success = false;
        return HU_ERR_PROVIDER_RESPONSE;
    }

    size_t id_len = id_val->data.string.len < sizeof(out->voice_id) - 1 ? id_val->data.string.len
                                                                        : sizeof(out->voice_id) - 1;
    memcpy(out->voice_id, id_val->data.string.ptr, id_len);
    out->voice_id[id_len] = '\0';

    hu_json_value_t *name_val = hu_json_object_get(parsed, "name");
    if (name_val && name_val->type == HU_JSON_STRING) {
        size_t nlen = name_val->data.string.len < sizeof(out->name) - 1 ? name_val->data.string.len
                                                                        : sizeof(out->name) - 1;
        memcpy(out->name, name_val->data.string.ptr, nlen);
        out->name[nlen] = '\0';
    }

    hu_json_value_t *lang_val = hu_json_object_get(parsed, "language");
    if (lang_val && lang_val->type == HU_JSON_STRING) {
        size_t llen = lang_val->data.string.len < sizeof(out->language) - 1
                          ? lang_val->data.string.len
                          : sizeof(out->language) - 1;
        memcpy(out->language, lang_val->data.string.ptr, llen);
        out->language[llen] = '\0';
    }

    out->success = true;
    hu_json_free(alloc, parsed);
    return HU_OK;
}
#endif /* HU_HTTP_CURL && !HU_IS_TEST */

hu_error_t hu_voice_clone_from_file(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                    const char *file_path, const hu_voice_clone_config_t *cfg,
                                    hu_voice_clone_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!api_key || api_key_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!file_path || file_path[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)api_key;
    (void)api_key_len;
    (void)file_path;
    (void)cfg;
    snprintf(out->voice_id, sizeof(out->voice_id), "test-clone-%08x", 0xDEADBEEFu);
    snprintf(out->name, sizeof(out->name), "test-voice");
    snprintf(out->language, sizeof(out->language), "en");
    out->success = true;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    /* Verify file exists */
    if (access(file_path, R_OK) != 0)
        return HU_ERR_IO;

    hu_voice_clone_config_t defaults;
    hu_voice_clone_config_default(&defaults);
    if (!cfg)
        cfg = &defaults;

    const char *name = (cfg->name && cfg->name_len > 0) ? cfg->name : "Human Voice Clone";
    const char *lang = (cfg->language && cfg->language_len > 0) ? cfg->language : "en";

    char hdr_api[256];
    int n = snprintf(hdr_api, sizeof(hdr_api), "X-API-Key: %.*s", (int)api_key_len, api_key);
    if (n < 0 || (size_t)n >= sizeof(hdr_api))
        return HU_ERR_INVALID_ARGUMENT;

    char hdr_ver[64];
    snprintf(hdr_ver, sizeof(hdr_ver), "Cartesia-Version: %s", CARTESIA_VERSION);

    /* Build curl multipart form args */
    size_t clip_cap = 64 + strlen(file_path);
    char *clip_arg = (char *)alloc->alloc(alloc->ctx, clip_cap);
    if (!clip_arg)
        return HU_ERR_OUT_OF_MEMORY;
    n = snprintf(clip_arg, clip_cap, "clip=@%s", file_path);
    if (n < 0 || (size_t)n >= clip_cap) {
        alloc->free(alloc->ctx, clip_arg, clip_cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    char name_arg[256];
    snprintf(name_arg, sizeof(name_arg), "name=%s", name);

    char lang_arg[32];
    snprintf(lang_arg, sizeof(lang_arg), "language=%s", lang);

    const char *argv[24];
    size_t argc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-s";
    argv[argc++] = "-X";
    argv[argc++] = "POST";
    argv[argc++] = "-H";
    argv[argc++] = hdr_api;
    argv[argc++] = "-H";
    argv[argc++] = hdr_ver;
    argv[argc++] = "-F";
    argv[argc++] = clip_arg;
    argv[argc++] = "-F";
    argv[argc++] = name_arg;
    argv[argc++] = "-F";
    argv[argc++] = lang_arg;

    char desc_arg[512];
    if (cfg->description && cfg->description_len > 0) {
        snprintf(desc_arg, sizeof(desc_arg), "description=%.*s", (int)cfg->description_len,
                 cfg->description);
        argv[argc++] = "-F";
        argv[argc++] = desc_arg;
    }

    char base_arg[128];
    if (cfg->base_voice_id && cfg->base_voice_id_len > 0) {
        snprintf(base_arg, sizeof(base_arg), "base_voice_id=%.*s", (int)cfg->base_voice_id_len,
                 cfg->base_voice_id);
        argv[argc++] = "-F";
        argv[argc++] = base_arg;
    }

    argv[argc++] = CARTESIA_CLONE_URL;
    argv[argc] = NULL;

    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 4 * 1024 * 1024, &result);
    alloc->free(alloc->ctx, clip_arg, clip_cap);

    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf || result.stdout_len == 0) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    err = parse_clone_response(alloc, result.stdout_buf, result.stdout_len, out);
    hu_run_result_free(alloc, &result);
    return err;
#else
    (void)api_key;
    (void)api_key_len;
    (void)file_path;
    (void)cfg;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_voice_clone_from_bytes(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                     const uint8_t *audio, size_t audio_len, const char *mime_type,
                                     const hu_voice_clone_config_t *cfg,
                                     hu_voice_clone_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!api_key || api_key_len == 0 || !audio || audio_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)api_key;
    (void)api_key_len;
    (void)audio;
    (void)audio_len;
    (void)mime_type;
    snprintf(out->voice_id, sizeof(out->voice_id), "test-clone-%08x", 0xCAFEBABEu);
    if (cfg && cfg->name && cfg->name_len > 0)
        snprintf(out->name, sizeof(out->name), "%.*s", (int)cfg->name_len, cfg->name);
    else
        snprintf(out->name, sizeof(out->name), "test-voice-bytes");
    snprintf(out->language, sizeof(out->language), "en");
    out->success = true;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    /* Write bytes to temp file, then delegate to from_file */
    const char *ext = ".wav";
    if (mime_type) {
        if (strstr(mime_type, "mpeg") || strstr(mime_type, "mp3"))
            ext = ".mp3";
        else if (strstr(mime_type, "m4a") || strstr(mime_type, "mp4"))
            ext = ".m4a";
        else if (strstr(mime_type, "ogg"))
            ext = ".ogg";
        else if (strstr(mime_type, "caf"))
            ext = ".caf";
    }

    char *tmp_dir = hu_platform_get_temp_dir(alloc);
    if (!tmp_dir)
        return HU_ERR_IO;

    char tmpl[512];
    int n = snprintf(tmpl, sizeof(tmpl), "%s/human_clone_XXXXXX%s", tmp_dir, ext);
    alloc->free(alloc->ctx, tmp_dir, strlen(tmp_dir) + 1);
    if (n < 0 || (size_t)n >= sizeof(tmpl))
        return HU_ERR_INVALID_ARGUMENT;

    int fd = mkstemps(tmpl, (int)strlen(ext));
    if (fd < 0)
        return HU_ERR_IO;

    const uint8_t *p = audio;
    size_t left = audio_len;
    while (left > 0) {
        ssize_t w = write(fd, p, left > 65536 ? 65536 : left);
        if (w <= 0) {
            close(fd);
            unlink(tmpl);
            return HU_ERR_IO;
        }
        p += (size_t)w;
        left -= (size_t)w;
    }
    close(fd);

    hu_error_t err = hu_voice_clone_from_file(alloc, api_key, api_key_len, tmpl, cfg, out);
    unlink(tmpl);
    return err;
#else
    (void)api_key;
    (void)api_key_len;
    (void)audio;
    (void)audio_len;
    (void)mime_type;
    (void)cfg;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

#else /* !HU_ENABLE_CARTESIA */

hu_error_t hu_voice_clone_from_file(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                    const char *file_path, const hu_voice_clone_config_t *cfg,
                                    hu_voice_clone_result_t *out) {
    (void)alloc;
    (void)api_key;
    (void)api_key_len;
    (void)file_path;
    (void)cfg;
    if (out)
        memset(out, 0, sizeof(*out));
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_voice_clone_from_bytes(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                     const uint8_t *audio, size_t audio_len, const char *mime_type,
                                     const hu_voice_clone_config_t *cfg,
                                     hu_voice_clone_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!api_key || api_key_len == 0 || !audio || audio_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)mime_type;
    snprintf(out->voice_id, sizeof(out->voice_id), "test-clone-%08x", 0xCAFEBABEu);
    if (cfg && cfg->name && cfg->name_len > 0)
        snprintf(out->name, sizeof(out->name), "%.*s", (int)cfg->name_len, cfg->name);
    else
        snprintf(out->name, sizeof(out->name), "test-voice-bytes");
    snprintf(out->language, sizeof(out->language), "en");
    out->success = true;
    return HU_OK;
#else
    (void)api_key;
    (void)api_key_len;
    (void)audio;
    (void)audio_len;
    (void)mime_type;
    (void)cfg;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

#endif /* HU_ENABLE_CARTESIA */

/* ── Persona voice_id writer ─────────────────────────────────────────── */

hu_error_t hu_persona_set_voice_id(hu_allocator_t *alloc, const char *persona_name, size_t name_len,
                                   const char *voice_id, size_t voice_id_len) {
    if (!alloc || !persona_name || name_len == 0 || !voice_id || voice_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    (void)alloc;
    (void)persona_name;
    (void)name_len;
    (void)voice_id;
    (void)voice_id_len;
    return HU_OK;
#elif defined(__unix__) || defined(__APPLE__)
    char base[512];
    if (!hu_persona_base_dir(base, sizeof(base)))
        return HU_ERR_IO;

    char path[768];
    int n = snprintf(path, sizeof(path), "%s/%.*s.json", base, (int)name_len, persona_name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    /* Read existing persona JSON */
    char *buf = NULL;
    size_t rd = 0;
    hu_error_t err = hu_file_slurp(alloc, path, 0, &buf, &rd);
    if (err == HU_ERR_OUT_OF_MEMORY)
        return HU_ERR_OUT_OF_MEMORY;
    if (err != HU_OK)
        return HU_ERR_IO;
    if (rd == 0) {
        alloc->free(alloc->ctx, buf, 1);
        return HU_ERR_PARSE;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, buf, rd, &root);
    alloc->free(alloc->ctx, buf, rd + 1);
    if (err != HU_OK)
        return HU_ERR_PARSE;

    /* Ensure "voice" object exists */
    hu_json_value_t *voice = hu_json_object_get(root, "voice");
    if (!voice || voice->type != HU_JSON_OBJECT) {
        voice = hu_json_object_new(alloc);
        if (!voice) {
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        hu_json_object_set(alloc, root, "voice", voice);
    }

    hu_json_object_set(alloc, voice, "voice_id", hu_json_string_new(alloc, voice_id, voice_id_len));
    hu_json_object_set(alloc, voice, "provider", hu_json_string_new(alloc, "cartesia", 8));

    /* Ensure voice_messages.enabled = true so the daemon actually uses this voice */
    hu_json_value_t *vm = hu_json_object_get(root, "voice_messages");
    if (!vm || vm->type != HU_JSON_OBJECT) {
        vm = hu_json_object_new(alloc);
        if (vm)
            hu_json_object_set(alloc, root, "voice_messages", vm);
    }
    if (vm && vm->type == HU_JSON_OBJECT)
        hu_json_object_set(alloc, vm, "enabled", hu_json_bool_new(alloc, true));

    /* Serialize and write back */
    char *json_out = NULL;
    size_t json_out_len = 0;
    err = hu_json_stringify(alloc, root, &json_out, &json_out_len);
    hu_json_free(alloc, root);
    if (err != HU_OK)
        return err;

    FILE *wf = fopen(path, "w");
    if (!wf) {
        alloc->free(alloc->ctx, json_out, json_out_len + 1);
        return HU_ERR_IO;
    }
    size_t written = fwrite(json_out, 1, json_out_len, wf);
    fclose(wf);
    alloc->free(alloc->ctx, json_out, json_out_len + 1);

    return written == json_out_len ? HU_OK : HU_ERR_IO;
#else
    (void)alloc;
    (void)persona_name;
    (void)name_len;
    (void)voice_id;
    (void)voice_id_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}
