#include "human/multimodal.h"
#include "human/core/local_only_guard.h"
#include "human/multimodal/audio.h"
#include "human/multimodal/video.h"
#include <string.h>

static bool match_prefix(const char *mime, size_t mime_len, const char *prefix, size_t prefix_len) {
    if (mime_len < prefix_len)
        return false;
    for (size_t i = 0; i < prefix_len; i++) {
        char a = (char)(mime[i] >= 'A' && mime[i] <= 'Z' ? mime[i] + 32 : mime[i]);
        char b = (char)(prefix[i] >= 'A' && prefix[i] <= 'Z' ? prefix[i] + 32 : prefix[i]);
        if (a != b)
            return false;
    }
    return true;
}

hu_error_t hu_multimodal_detect_type(const char *mime, size_t mime_len, hu_modality_t *out) {
    if (!mime || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (match_prefix(mime, mime_len, "image/", 6)) {
        *out = HU_MODALITY_IMAGE;
        return HU_OK;
    }
    if (match_prefix(mime, mime_len, "audio/", 6)) {
        *out = HU_MODALITY_AUDIO;
        return HU_OK;
    }
    if (match_prefix(mime, mime_len, "video/", 6)) {
        *out = HU_MODALITY_VIDEO;
        return HU_OK;
    }
    *out = HU_MODALITY_TEXT;
    return HU_OK;
}

hu_error_t hu_multimodal_needs_fallback(const hu_provider_capabilities_t *caps, hu_modality_t type,
                                        bool *needs_fallback) {
    if (!caps || !needs_fallback)
        return HU_ERR_INVALID_ARGUMENT;
    switch (type) {
    case HU_MODALITY_TEXT:
        *needs_fallback = false;
        break;
    case HU_MODALITY_IMAGE:
        *needs_fallback = !caps->supports_image;
        break;
    case HU_MODALITY_AUDIO:
        *needs_fallback = !caps->supports_audio;
        break;
    case HU_MODALITY_VIDEO:
        *needs_fallback = !caps->supports_video;
        break;
    default:
        *needs_fallback = true;
        break;
    }
    return HU_OK;
}

const char *hu_modality_name(hu_modality_t type) {
    switch (type) {
    case HU_MODALITY_TEXT:
        return "text";
    case HU_MODALITY_IMAGE:
        return "image";
    case HU_MODALITY_AUDIO:
        return "audio";
    case HU_MODALITY_VIDEO:
        return "video";
    default:
        return "text";
    }
}

static int path_to_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool path_match_ext(const char *filename, size_t filename_len, const char *ext,
                           size_t ext_len) {
    if (filename_len < ext_len + 1)
        return false;
    if (filename[filename_len - ext_len - 1] != '.')
        return false;
    for (size_t i = 0; i < ext_len; i++) {
        if (path_to_lower((unsigned char)filename[filename_len - ext_len + i]) !=
            (unsigned char)ext[i])
            return false;
    }
    return true;
}

static bool path_is_audio_media(const char *path, size_t path_len) {
    return path_match_ext(path, path_len, "mp3", 3) || path_match_ext(path, path_len, "wav", 3) ||
           path_match_ext(path, path_len, "ogg", 3) || path_match_ext(path, path_len, "m4a", 3) ||
           path_match_ext(path, path_len, "caf", 3);
}

static bool path_is_video_media(const char *path, size_t path_len) {
    return path_match_ext(path, path_len, "mp4", 3) || path_match_ext(path, path_len, "mov", 3) ||
           path_match_ext(path, path_len, "webm", 4) || path_match_ext(path, path_len, "avi", 3);
}

hu_error_t hu_multimodal_route_local_media(hu_allocator_t *alloc, const char *file_path,
                                           size_t path_len, hu_provider_t *provider,
                                           const char *model, size_t model_len, char **out_text,
                                           size_t *out_text_len) {
    if (!alloc || !file_path || path_len == 0 || !provider || !out_text || !out_text_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_text_len = 0;

    hu_error_t err = HU_ERR_INVALID_ARGUMENT;
    const char *lo_prev = hu_local_only_set_caller("inbound_media");
    if (path_is_audio_media(file_path, path_len))
        err = hu_multimodal_process_audio(alloc, file_path, path_len, provider, model, model_len,
                                          out_text, out_text_len);
    else if (path_is_video_media(file_path, path_len))
        err = hu_multimodal_process_video(alloc, file_path, path_len, provider, model, model_len,
                                          out_text, out_text_len);
    (void)hu_local_only_set_caller(lo_prev);
    return err;
}
