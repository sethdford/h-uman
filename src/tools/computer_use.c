/* computer_use — macOS screen control via CoreGraphics (screenshot, click, type, scroll, keys). */
#include "human/tools/computer_use.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/file.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/multimodal.h"
#include "human/tool.h"
#include "human/tools/validation.h"
#include "human/tools/visual_grounding.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

#if defined(__APPLE__) && !defined(HU_IS_TEST)
#include "human/core/process_util.h"
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <ImageIO/ImageIO.h>
#include <unistd.h>
#endif

#define HU_CU_NAME      "computer_use"
#define HU_CU_TEXT_MAX  4096
#define HU_CU_COMBO_MAX 128

#define HU_CU_PARAMS                                                                          \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":["         \
    "\"screenshot\","                                                                         \
    "\"click\",\"type\",\"scroll\",\"key\"]},\"x\":{\"type\":\"number\"},\"y\":{\"type\":"    \
    "\"number\"},"                                                                            \
    "\"target\":{\"type\":\"string\",\"description\":\"Natural language description of UI "   \
    "element "                                                                                \
    "to interact with (uses vision to locate)\"},"                                            \
    "\"text\":{\"type\":\"string\"},\"direction\":{\"type\":\"string\"},\"delta\":{\"type\":" \
    "\"number\"},"                                                                            \
    "\"combo\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"action\"]}"

typedef struct hu_computer_use_ctx {
    hu_security_policy_t *policy;
    hu_provider_t *ground_provider;
    const char *ground_model;
    size_t ground_model_len;
} hu_computer_use_ctx_t;

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static bool cu_autonomy_allows(hu_computer_use_ctx_t *c) {
    if (!c || !c->policy)
        return false;
    return (int)c->policy->autonomy >= (int)HU_AUTONOMY_SUPERVISED;
}
#endif

__attribute__((unused)) static char *cu_dup_json(hu_allocator_t *alloc, const char *s, size_t len) {
    char *p = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
#if (defined(__APPLE__) && !defined(HU_IS_TEST)) || \
    (defined(__linux__) && !defined(HU_IS_TEST) && defined(HU_HAS_X11) && HU_HAS_X11)
static bool cu_path_json_safe(const char *path) {
    if (!path)
        return true;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p == '"' || *p == '\\' || *p < 0x20)
            return false;
    }
    return true;
}

static void cu_clamp_pointer_coords(double *x, double *y) {
    if (!x || !y)
        return;
    if (*x < -32768.0)
        *x = -32768.0;
    if (*x > 32767.0)
        *x = 32767.0;
    if (*y < -32768.0)
        *y = -32768.0;
    if (*y > 32767.0)
        *y = 32767.0;
}
#endif
#endif

#if defined(__APPLE__) && !defined(HU_IS_TEST)

/* CGWindowListCreateImage is marked unavailable in the macOS 15 SDK; load dynamically so we
 * still call CoreGraphics when the symbol exists at runtime. */
typedef CGImageRef (*hu_cu_window_list_create_image_fn)(CGRect, CGWindowListOption, CGWindowID,
                                                        CGWindowImageOption);

static CGImageRef cu_try_cg_window_capture(void) {
    static hu_cu_window_list_create_image_fn impl;
    static unsigned char loaded;
    if (!loaded) {
        loaded = 1;
        void *sym = dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
        impl = (hu_cu_window_list_create_image_fn)sym;
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    }
    if (!impl)
        return NULL;
    return impl(CGRectInfinite, kCGWindowListOptionOnScreenOnly, kCGNullWindowID,
                kCGWindowImageDefault);
}

static bool cu_screencapture_to_path(hu_allocator_t *alloc, hu_computer_use_ctx_t *c,
                                     const char *path) {
    const char *argv[6];
    argv[0] = "screencapture";
    argv[1] = "-x";
    argv[2] = "-t";
    argv[3] = "png";
    argv[4] = path;
    argv[5] = NULL;
    hu_run_result_t run = {0};
    hu_error_t e = hu_process_run_with_policy(alloc, argv, NULL, 4096, c ? c->policy : NULL, &run);
    bool ok = (e == HU_OK && run.success);
    hu_run_result_free(alloc, &run);
    return ok;
}

static bool cu_read_png_file(const char *path, CFMutableDataRef into) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        CFDataAppendBytes(into, buf, (CFIndex)n);
    fclose(f);
    return CFDataGetLength(into) > 0;
}

static CGKeyCode cu_letter_keycode(char lc) {
    static const uint8_t map[26] = {
        0x00, 0x0b, 0x08, 0x02, 0x0e, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2e,
        0x2d, 0x1f, 0x23, 0x0c, 0x0f, 0x01, 0x11, 0x20, 0x09, 0x0d, 0x07, 0x10, 0x06,
    };
    if (lc >= 'a' && lc <= 'z')
        return (CGKeyCode)map[(size_t)(lc - 'a')];
    return (CGKeyCode)0xffff;
}

static CGKeyCode cu_digit_keycode(char d) {
    static const uint8_t digits[10] = {0x1d, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1a, 0x1c, 0x19};
    if (d >= '0' && d <= '9')
        return (CGKeyCode)digits[(size_t)(d - '0')];
    return (CGKeyCode)0xffff;
}

static bool cu_token_to_keycode(const char *tok, CGKeyCode *kc) {
    if (!tok || !tok[0])
        return false;
    if (strlen(tok) == 1) {
        char c = tok[0];
        if (c >= 'A' && c <= 'Z')
            c = (char)tolower((unsigned char)c);
        if (c >= 'a' && c <= 'z') {
            *kc = cu_letter_keycode(c);
            return *kc != (CGKeyCode)0xffff;
        }
        if (c >= '0' && c <= '9') {
            *kc = cu_digit_keycode(c);
            return *kc != (CGKeyCode)0xffff;
        }
        if (c == ' ')
            *kc = (CGKeyCode)0x31;
        else if (c == '\t')
            *kc = (CGKeyCode)0x30;
        else
            return false;
        return true;
    }
    if (strcasecmp(tok, "space") == 0) {
        *kc = (CGKeyCode)0x31;
        return true;
    }
    if (strcasecmp(tok, "return") == 0 || strcasecmp(tok, "enter") == 0) {
        *kc = (CGKeyCode)0x24;
        return true;
    }
    if (strcasecmp(tok, "tab") == 0) {
        *kc = (CGKeyCode)0x30;
        return true;
    }
    if (strcasecmp(tok, "escape") == 0 || strcasecmp(tok, "esc") == 0) {
        *kc = (CGKeyCode)0x35;
        return true;
    }
    return false;
}

static void cu_trim_inplace(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start))
        start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    if (start != s)
        memmove(s, start, (size_t)(end - start) + 1);
}

static bool cu_parse_combo(const char *combo, CGEventFlags *out_flags, CGKeyCode *out_kc) {
    char buf[HU_CU_COMBO_MAX];
    size_t clen = combo ? strlen(combo) : 0;
    if (!combo || clen == 0 || clen >= sizeof(buf))
        return false;
    memcpy(buf, combo, clen);
    buf[clen] = '\0';

    CGEventFlags flags = 0;
    CGKeyCode kc = (CGKeyCode)0xffff;
    int keys = 0;
    char *save = NULL;

    for (char *t = strtok_r(buf, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
        cu_trim_inplace(t);
        if (!t[0])
            continue;
        if (strcasecmp(t, "cmd") == 0 || strcasecmp(t, "command") == 0 ||
            strcasecmp(t, "meta") == 0) {
            flags |= kCGEventFlagMaskCommand;
            continue;
        }
        if (strcasecmp(t, "shift") == 0) {
            flags |= kCGEventFlagMaskShift;
            continue;
        }
        if (strcasecmp(t, "alt") == 0 || strcasecmp(t, "option") == 0) {
            flags |= kCGEventFlagMaskAlternate;
            continue;
        }
        if (strcasecmp(t, "ctrl") == 0 || strcasecmp(t, "control") == 0) {
            flags |= kCGEventFlagMaskControl;
            continue;
        }
        if (!cu_token_to_keycode(t, &kc))
            return false;
        keys++;
    }
    if (keys != 1 || kc == (CGKeyCode)0xffff)
        return false;
    *out_flags = flags;
    *out_kc = kc;
    return true;
}

static void cu_post_key_combo(CGKeyCode kc, CGEventFlags flags) {
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, kc, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, kc, false);
    if (down) {
        CGEventSetFlags(down, flags);
        CGEventPost(kCGHIDEventTap, down);
        CFRelease(down);
    }
    if (up) {
        CGEventSetFlags(up, flags);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
    }
}

static hu_error_t cu_mac_screenshot(hu_allocator_t *alloc, hu_computer_use_ctx_t *c,
                                    const char *path, hu_tool_result_t *out) {
    CGImageRef img = cu_try_cg_window_capture();

    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
    if (!data) {
        if (img)
            CGImageRelease(img);
        *out = hu_tool_result_fail("screenshot encode failed", 24);
        return HU_OK;
    }

    if (path && path[0]) {
        if (!cu_path_json_safe(path)) {
            CFRelease(data);
            if (img)
                CGImageRelease(img);
            *out = hu_tool_result_fail("path contains invalid characters", 31);
            return HU_OK;
        }
        const char *ws = c && c->policy ? c->policy->workspace_dir : NULL;
        size_t ws_len = ws ? strlen(ws) : 0;
        if (hu_tool_validate_path(path, ws, ws_len) != HU_OK) {
            CFRelease(data);
            if (img)
                CGImageRelease(img);
            *out = hu_tool_result_fail("path not allowed", 16);
            return HU_OK;
        }
    }

    if (img && path && path[0]) {
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                               (CFIndex)strlen(path), false);
        if (!url) {
            CFRelease(data);
            CGImageRelease(img);
            *out = hu_tool_result_fail("path invalid", 12);
            return HU_OK;
        }
        CGImageDestinationRef dest =
            CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        CFRelease(url);
        if (!dest) {
            CFRelease(data);
            CGImageRelease(img);
            *out = hu_tool_result_fail("screenshot write failed", 23);
            return HU_OK;
        }
        CGImageDestinationAddImage(dest, img, NULL);
        if (!CGImageDestinationFinalize(dest)) {
            CFRelease(dest);
            CFRelease(data);
            CGImageRelease(img);
            *out = hu_tool_result_fail("screenshot write failed", 23);
            return HU_OK;
        }
        CFRelease(dest);
    }

    if (img) {
        CGImageDestinationRef dest_d =
            CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, NULL);
        if (!dest_d) {
            CFRelease(data);
            CGImageRelease(img);
            *out = hu_tool_result_fail("screenshot encode failed", 24);
            return HU_OK;
        }
        CGImageDestinationAddImage(dest_d, img, NULL);
        if (!CGImageDestinationFinalize(dest_d)) {
            CFRelease(dest_d);
            CFRelease(data);
            CGImageRelease(img);
            *out = hu_tool_result_fail("screenshot encode failed", 24);
            return HU_OK;
        }
        CFRelease(dest_d);
        CGImageRelease(img);
    } else {
        /* CoreGraphics capture unavailable — fall back to screencapture(1). */
        if (path && path[0]) {
            if (!cu_screencapture_to_path(alloc, c, path) || !cu_read_png_file(path, data)) {
                CFRelease(data);
                *out = hu_tool_result_fail("screenshot capture failed", 25);
                return HU_OK;
            }
        } else {
            char tmpl[] = "/tmp/hu_cuXXXXXX.png";
            int fd = mkstemps(tmpl, 4);
            if (fd < 0) {
                CFRelease(data);
                *out = hu_tool_result_fail("screenshot capture failed", 25);
                return HU_OK;
            }
            (void)close(fd);
            if (unlink(tmpl) != 0) {
                CFRelease(data);
                *out = hu_tool_result_fail("screenshot capture failed", 25);
                return HU_OK;
            }
            if (!cu_screencapture_to_path(alloc, c, tmpl) || !cu_read_png_file(tmpl, data)) {
                (void)unlink(tmpl);
                CFRelease(data);
                *out = hu_tool_result_fail("screenshot capture failed", 25);
                return HU_OK;
            }
            (void)unlink(tmpl);
        }
    }

    const UInt8 *bytes = CFDataGetBytePtr(data);
    CFIndex nbytes = CFDataGetLength(data);
    if (!bytes || nbytes <= 0) {
        CFRelease(data);
        *out = hu_tool_result_fail("screenshot empty", 16);
        return HU_OK;
    }

    char *b64 = NULL;
    size_t b64_len = 0;
    hu_error_t enc = hu_multimodal_encode_base64(alloc, bytes, (size_t)nbytes, &b64, &b64_len);
    CFRelease(data);
    if (enc != HU_OK || !b64) {
        if (b64)
            alloc->free(alloc->ctx, b64, b64_len + 1);
        *out = hu_tool_result_fail("base64 encode failed", 20);
        return HU_OK;
    }

    if (path && path[0]) {
        size_t plen = strlen(path);
        hu_json_buf_t jb = {0};
        hu_error_t jerr = hu_json_buf_init(&jb, alloc);
        if (jerr != HU_OK) {
            alloc->free(alloc->ctx, b64, b64_len + 1);
            *out = hu_tool_result_fail("out of memory", 13);
            return jerr;
        }
        jerr = hu_json_buf_append_raw(&jb, "{", 1);
        if (jerr == HU_OK)
            jerr = hu_json_append_key_value(&jb, "format", 6, "png", 3);
        if (jerr == HU_OK)
            jerr = hu_json_buf_append_raw(&jb, ",", 1);
        if (jerr == HU_OK)
            jerr = hu_json_append_key_value(&jb, "path", 4, path, plen);
        if (jerr == HU_OK)
            jerr = hu_json_buf_append_raw(&jb, ",", 1);
        if (jerr == HU_OK)
            jerr = hu_json_append_key_value(&jb, "base64", 6, b64, b64_len);
        if (jerr == HU_OK)
            jerr = hu_json_buf_append_raw(&jb, "}", 1);
        alloc->free(alloc->ctx, b64, b64_len + 1);
        if (jerr != HU_OK) {
            hu_json_buf_free(&jb);
            *out = hu_tool_result_fail("failed to build screenshot json", 31);
            return jerr == HU_ERR_OUT_OF_MEMORY ? jerr : HU_OK;
        }
        *out = hu_tool_result_ok_owned(jb.ptr, jb.len);
        jb.ptr = NULL;
        jb.len = 0;
        jb.cap = 0;
        hu_json_buf_free(&jb);
        return HU_OK;
    }

    size_t need = 32 + b64_len;
    char *json = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!json) {
        alloc->free(alloc->ctx, b64, b64_len + 1);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(json, need + 1, "{\"format\":\"png\",\"base64\":\"%.*s\"}", (int)b64_len, b64);
    alloc->free(alloc->ctx, b64, b64_len + 1);
    if (n <= 0 || (size_t)n > need) {
        alloc->free(alloc->ctx, json, need + 1);
        *out = hu_tool_result_fail("output overflow", 15);
        return HU_OK;
    }
    *out = hu_tool_result_ok_owned(json, (size_t)n);
    return HU_OK;
}

static hu_error_t cu_mac_click(hu_allocator_t *alloc, double x, double y, hu_tool_result_t *out) {
    cu_clamp_pointer_coords(&x, &y);
    CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);
    CGEventRef down =
        CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDown, point, kCGMouseButtonLeft);
    CGEventRef up = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp, point, kCGMouseButtonLeft);
    if (down)
        CGEventPost(kCGHIDEventTap, down);
    if (up)
        CGEventPost(kCGHIDEventTap, up);
    if (down)
        CFRelease(down);
    if (up)
        CFRelease(up);
    char *j = cu_dup_json(alloc, "{\"success\":true}", 16);
    if (!j) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(j, 16);
    return HU_OK;
}

#endif /* __APPLE__ && !HU_IS_TEST */

#if defined(__linux__) && !defined(HU_IS_TEST) && defined(HU_HAS_X11) && HU_HAS_X11

#include "human/core/process_util.h"
#include <fcntl.h>
#include <unistd.h>
#include <X11/extensions/XTest.h>
#include <X11/Xlib.h>

static bool cu_linux_png_nonempty(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    unsigned char b[1];
    size_t n = fread(b, 1, 1, f);
    fclose(f);
    return n == 1;
}

static bool cu_linux_try_screenshot_cmd(hu_allocator_t *alloc, hu_security_policy_t *policy,
                                        const char *path) {
    const char *const v1[] = {"gnome-screenshot", "-f", path, NULL};
    hu_run_result_t r1 = {0};
    hu_error_t e1 = hu_process_run_with_policy(alloc, v1, NULL, 256, policy, &r1);
    bool ok1 = (e1 == HU_OK && r1.success && r1.exit_code == 0);
    hu_run_result_free(alloc, &r1);
    if (ok1 && cu_linux_png_nonempty(path))
        return true;

    const char *const v2[] = {"scrot", path, NULL};
    hu_run_result_t r2 = {0};
    hu_error_t e2 = hu_process_run_with_policy(alloc, v2, NULL, 256, policy, &r2);
    bool ok2 = (e2 == HU_OK && r2.success && r2.exit_code == 0);
    hu_run_result_free(alloc, &r2);
    if (ok2 && cu_linux_png_nonempty(path))
        return true;

    const char *const v3[] = {"import", "-window", "root", path, NULL};
    hu_run_result_t r3 = {0};
    hu_error_t e3 = hu_process_run_with_policy(alloc, v3, NULL, 256, policy, &r3);
    bool ok3 = (e3 == HU_OK && r3.success && r3.exit_code == 0);
    hu_run_result_free(alloc, &r3);
    return ok3 && cu_linux_png_nonempty(path);
}

/* Reads the whole PNG into a buffer; caller frees with *out_len + 1 (the
 * hu_file_slurp NUL terminator). Missing/empty/oversize files all map to
 * HU_ERR_IO to preserve the original single-error contract. */
static hu_error_t cu_linux_read_png_file(hu_allocator_t *alloc, const char *path,
                                         unsigned char **out, size_t *out_len) {
    char *data = NULL;
    size_t len = 0;
    hu_error_t err = hu_file_slurp(alloc, path, (size_t)(16u * 1024u * 1024u), &data, &len);
    if (err == HU_ERR_OUT_OF_MEMORY)
        return err;
    if (err != HU_OK)
        return HU_ERR_IO;
    if (len == 0) { /* slurp accepts empty files; a 0-byte PNG is a failed capture */
        alloc->free(alloc->ctx, data, 1);
        return HU_ERR_IO;
    }
    *out = (unsigned char *)data;
    *out_len = len;
    return HU_OK;
}

static hu_error_t cu_linux_screenshot(hu_allocator_t *alloc, hu_computer_use_ctx_t *c,
                                      const char *path, hu_tool_result_t *out) {
    if (path && path[0]) {
        if (!cu_path_json_safe(path)) {
            *out = hu_tool_result_fail("path contains invalid characters", 31);
            return HU_OK;
        }
        const char *ws = c && c->policy ? c->policy->workspace_dir : NULL;
        size_t ws_len = ws ? strlen(ws) : 0;
        if (hu_tool_validate_path(path, ws, ws_len) != HU_OK) {
            *out = hu_tool_result_fail("path not allowed", 16);
            return HU_OK;
        }
    }
    char tmpl[] = "/tmp/hu_cuXXXXXX.png";
    const char *work = path;
    int tmp_fd = -1;
    int need_unlink = 0;
    if (!path || !path[0]) {
        tmp_fd = mkstemps(tmpl, 4);
        if (tmp_fd < 0) {
            *out = hu_tool_result_fail("screenshot capture failed", 25);
            return HU_OK;
        }
        (void)close(tmp_fd);
        work = tmpl;
        need_unlink = 1;
    }

    if (!cu_linux_try_screenshot_cmd(alloc, c ? c->policy : NULL, work)) {
        if (need_unlink)
            (void)unlink(tmpl);
        *out = hu_tool_result_fail(
            "screenshot failed: install gnome-screenshot, scrot, or imagemagick import", 76);
        return HU_OK;
    }

    unsigned char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t rr = cu_linux_read_png_file(alloc, work, &raw, &raw_len);
    if (need_unlink)
        (void)unlink(tmpl);
    if (rr != HU_OK || !raw) {
        if (raw)
            alloc->free(alloc->ctx, raw, raw_len + 1);
        *out = hu_tool_result_fail("screenshot read failed", 22);
        return HU_OK;
    }

    char *b64 = NULL;
    size_t b64_len = 0;
    hu_error_t enc = hu_multimodal_encode_base64(alloc, raw, raw_len, &b64, &b64_len);
    alloc->free(alloc->ctx, raw, raw_len + 1);
    if (enc != HU_OK || !b64) {
        if (b64)
            alloc->free(alloc->ctx, b64, b64_len + 1);
        *out = hu_tool_result_fail("base64 encode failed", 20);
        return enc != HU_OK ? enc : HU_ERR_OUT_OF_MEMORY;
    }

    if (path && path[0]) {
        size_t plen = strlen(path);
        hu_json_buf_t jb = {0};
        hu_error_t jerr = hu_json_buf_init(&jb, alloc);
        if (jerr != HU_OK) {
            alloc->free(alloc->ctx, b64, b64_len + 1);
            *out = hu_tool_result_fail("out of memory", 13);
            return jerr;
        }
        jerr = hu_json_buf_append_raw(&jb, "{", 1);
        if (jerr == HU_OK)
            jerr = hu_json_append_key_value(&jb, "format", 6, "png", 3);
        if (jerr == HU_OK)
            jerr = hu_json_buf_append_raw(&jb, ",", 1);
        if (jerr == HU_OK)
            jerr = hu_json_append_key_value(&jb, "path", 4, path, plen);
        if (jerr == HU_OK)
            jerr = hu_json_buf_append_raw(&jb, ",", 1);
        if (jerr == HU_OK)
            jerr = hu_json_append_key_value(&jb, "base64", 6, b64, b64_len);
        if (jerr == HU_OK)
            jerr = hu_json_buf_append_raw(&jb, "}", 1);
        alloc->free(alloc->ctx, b64, b64_len + 1);
        if (jerr != HU_OK) {
            hu_json_buf_free(&jb);
            *out = hu_tool_result_fail("failed to build screenshot json", 31);
            return jerr == HU_ERR_OUT_OF_MEMORY ? jerr : HU_OK;
        }
        *out = hu_tool_result_ok_owned(jb.ptr, jb.len);
        jb.ptr = NULL;
        jb.len = 0;
        jb.cap = 0;
        hu_json_buf_free(&jb);
        return HU_OK;
    }

    size_t need = 32 + b64_len;
    char *json = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!json) {
        alloc->free(alloc->ctx, b64, b64_len + 1);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(json, need + 1, "{\"format\":\"png\",\"base64\":\"%.*s\"}", (int)b64_len, b64);
    alloc->free(alloc->ctx, b64, b64_len + 1);
    if (n <= 0 || (size_t)n > need) {
        alloc->free(alloc->ctx, json, need + 1);
        *out = hu_tool_result_fail("output overflow", 15);
        return HU_OK;
    }
    *out = hu_tool_result_ok_owned(json, (size_t)n);
    return HU_OK;
}

static hu_error_t cu_linux_click(hu_allocator_t *alloc, double x, double y, hu_tool_result_t *out) {
    (void)alloc;
    cu_clamp_pointer_coords(&x, &y);
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        *out = hu_tool_result_fail("cannot open X11 display", 23);
        return HU_OK;
    }
    XTestFakeMotionEvent(dpy, -1, (int)x, (int)y, CurrentTime);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XFlush(dpy);
    XCloseDisplay(dpy);
    char *j = cu_dup_json(alloc, "{\"success\":true}", 16);
    if (!j) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(j, 16);
    return HU_OK;
}

static hu_error_t cu_linux_type(hu_allocator_t *alloc, hu_security_policy_t *policy,
                                const char *text, hu_tool_result_t *out) {
    const char *const argv[] = {"xdotool", "type", "--clearmodifiers", "--", text, NULL};
    hu_run_result_t run = {0};
    hu_error_t e = hu_process_run_with_policy(alloc, argv, NULL, 4096, policy, &run);
    bool ok = (e == HU_OK && run.success && run.exit_code == 0);
    hu_run_result_free(alloc, &run);
    if (!ok) {
        *out = hu_tool_result_fail("xdotool type failed (install xdotool)", 38);
        return HU_OK;
    }
    char *j = cu_dup_json(alloc, "{\"success\":true}", 16);
    if (!j) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(j, 16);
    return HU_OK;
}

static hu_error_t cu_linux_scroll(hu_allocator_t *alloc, double x, double y, int32_t scroll_delta,
                                  hu_tool_result_t *out) {
    (void)alloc;
    cu_clamp_pointer_coords(&x, &y);
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        *out = hu_tool_result_fail("cannot open X11 display", 23);
        return HU_OK;
    }
    XTestFakeMotionEvent(dpy, -1, (int)x, (int)y, CurrentTime);
    unsigned int button = scroll_delta > 0 ? 4u : 5u;
    int clicks = scroll_delta > 0 ? scroll_delta : -scroll_delta;
    if (clicks <= 0)
        clicks = 1;
    if (clicks > 10)
        clicks = 10;
    for (int i = 0; i < clicks; i++) {
        XTestFakeButtonEvent(dpy, button, True, CurrentTime);
        XTestFakeButtonEvent(dpy, button, False, CurrentTime);
    }
    XFlush(dpy);
    XCloseDisplay(dpy);
    char *j = cu_dup_json(alloc, "{\"success\":true}", 16);
    if (!j) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(j, 16);
    return HU_OK;
}

#endif /* linux && X11 */

static hu_error_t computer_use_execute(void *ctx, hu_allocator_t *alloc,
                                       const hu_json_value_t *args, hu_tool_result_t *out) {
    hu_computer_use_ctx_t *c = (hu_computer_use_ctx_t *)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)c;
    if (!args || args->type != HU_JSON_OBJECT) {
        *out = hu_tool_result_fail("invalid arguments", 17);
        return HU_OK;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action || !action[0]) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }
    if (strcmp(action, "screenshot") == 0) {
        const char *mock = "{\"format\":\"png\",\"base64\":"
                           "\"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEh"
                           "QGAhKmMIQAAAABJRU5ErkJggg==\"}";
        size_t mlen = strlen(mock);
        char *copy = cu_dup_json(alloc, mock, mlen);
        if (!copy) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(copy, mlen);
        return HU_OK;
    }
    if (strcmp(action, "click") == 0) {
        double x = hu_json_get_number(args, "x", 0);
        double y = hu_json_get_number(args, "y", 0);
        const char *target = hu_json_get_string(args, "target");
        size_t tlen = target ? strlen(target) : 0;
        if (tlen > 512) {
            *out = hu_tool_result_fail("target too long", 15);
            return HU_OK;
        }
        if (tlen > 0) {
            double gx = 0, gy = 0;
            hu_error_t ge = hu_visual_ground_action(alloc, NULL, NULL, 0, "mock.png", 9, target,
                                                    tlen, &gx, &gy, NULL, NULL);
            if (ge == HU_OK && gx >= 0.0 && gy >= 0.0) {
                x = gx;
                y = gy;
            } /* else keep x,y from JSON (fallback when grounding fails) */
        }
        if (x == 0.0 && y == 0.0 && tlen > 0) {
            *out = hu_tool_result_fail("visual grounding failed", 22);
            return HU_OK;
        }
        char *copy = NULL;
        if (tlen > 0) {
            copy = hu_sprintf(alloc,
                              "{\"success\":true,\"via_visual_grounding\":true,\"x\":%.0f,"
                              "\"y\":%.0f}",
                              x, y);
        } else {
            copy = cu_dup_json(alloc, "{\"success\":true}", 16);
        }
        if (!copy) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(copy, strlen(copy));
        return HU_OK;
    }
    if (strcmp(action, "type") == 0 || strcmp(action, "scroll") == 0 ||
        strcmp(action, "key") == 0) {
        char *copy = cu_dup_json(alloc, "{\"success\":true}", 16);
        if (!copy) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(copy, 16);
        return HU_OK;
    }
    *out = hu_tool_result_fail("unknown action", 14);
    return HU_OK;
#else
    if (!cu_autonomy_allows(c)) {
        *out = hu_tool_result_fail("computer_use requires supervised autonomy or higher", 49);
        return HU_OK;
    }

#if defined(__APPLE__)

    if (!args || args->type != HU_JSON_OBJECT) {
        *out = hu_tool_result_fail("invalid arguments", 17);
        return HU_OK;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action || !action[0]) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }

    if (strcmp(action, "screenshot") == 0) {
        const char *path = hu_json_get_string(args, "path");
        return cu_mac_screenshot(alloc, c, path, out);
    }

    if (strcmp(action, "click") == 0) {
        double x = hu_json_get_number(args, "x", 0);
        double y = hu_json_get_number(args, "y", 0);
        const char *target = hu_json_get_string(args, "target");
        size_t tlen = target ? strlen(target) : 0;
        if (tlen > 512) {
            *out = hu_tool_result_fail("target too long", 15);
            return HU_OK;
        }
        if (tlen > 0) {
            if (!c->ground_provider) {
                if (x == 0.0 && y == 0.0) {
                    *out = hu_tool_result_fail(
                        "visual grounding requires provider binding or explicit "
                        "coordinates",
                        58);
                    return HU_OK;
                }
            } else {
                char tmpl[] = "/tmp/hu_cu_vgXXXXXX.png";
                int fd = mkstemps(tmpl, 4);
                if (fd < 0) {
                    *out = hu_tool_result_fail("visual grounding temp file failed", 31);
                    return HU_OK;
                }
                (void)close(fd);
                (void)unlink(tmpl);
                if (!cu_screencapture_to_path(alloc, c, tmpl)) {
                    (void)unlink(tmpl);
                    *out = hu_tool_result_fail("visual grounding screenshot failed", 33);
                    return HU_OK;
                }
                double gx = -1.0, gy = -1.0;
                hu_error_t gerr = hu_visual_ground_action(
                    alloc, c->ground_provider, c->ground_model, c->ground_model_len, tmpl,
                    strlen(tmpl), target, tlen, &gx, &gy, NULL, NULL);
                (void)unlink(tmpl);
                if (gerr == HU_OK && gx >= 0.0 && gy >= 0.0) {
                    x = gx;
                    y = gy;
                }
            }
        }
        if (x == 0.0 && y == 0.0) {
            *out = hu_tool_result_fail("visual grounding failed", 22);
            return HU_OK;
        }
        return cu_mac_click(alloc, x, y, out);
    }

    if (strcmp(action, "type") == 0) {
        const char *text = hu_json_get_string(args, "text");
        if (!text) {
            *out = hu_tool_result_fail("missing text", 12);
            return HU_OK;
        }
        size_t tlen = strlen(text);
        if (tlen == 0 || tlen > HU_CU_TEXT_MAX) {
            *out = hu_tool_result_fail("text invalid", 12);
            return HU_OK;
        }
        CFStringRef str = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)text,
                                                  (CFIndex)tlen, kCFStringEncodingUTF8, false);
        if (!str) {
            *out = hu_tool_result_fail("text encode failed", 18);
            return HU_OK;
        }
        CFIndex len = CFStringGetLength(str);
        for (CFIndex i = 0; i < len; i++) {
            UniChar ch = CFStringGetCharacterAtIndex(str, i);
            CGEventRef ev_down = CGEventCreateKeyboardEvent(NULL, 0, true);
            CGEventRef ev_up = CGEventCreateKeyboardEvent(NULL, 0, false);
            if (ev_down) {
                CGEventKeyboardSetUnicodeString(ev_down, 1, &ch);
                CGEventPost(kCGHIDEventTap, ev_down);
                CFRelease(ev_down);
            }
            if (ev_up) {
                CGEventKeyboardSetUnicodeString(ev_up, 1, &ch);
                CGEventPost(kCGHIDEventTap, ev_up);
                CFRelease(ev_up);
            }
        }
        CFRelease(str);
        char *j = cu_dup_json(alloc, "{\"success\":true}", 16);
        if (!j) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(j, 16);
        return HU_OK;
    }

    if (strcmp(action, "scroll") == 0) {
        double x = hu_json_get_number(args, "x", 0);
        double y = hu_json_get_number(args, "y", 0);
        const char *dir = hu_json_get_string(args, "direction");
        double delta_num = hu_json_get_number(args, "delta", 0);
        int32_t scroll_delta = 0;
        if (delta_num > 0)
            scroll_delta = (int32_t)delta_num;
        else if (delta_num < 0)
            scroll_delta = (int32_t)delta_num;
        else if (dir) {
            if (strcasecmp(dir, "up") == 0)
                scroll_delta = 1;
            else if (strcasecmp(dir, "down") == 0)
                scroll_delta = -1;
        }
        if (scroll_delta == 0)
            scroll_delta = -1;
        cu_clamp_pointer_coords(&x, &y);
        CGEventRef scroll =
            CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, scroll_delta);
        if (scroll) {
            CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);
            CGEventSetLocation(scroll, point);
            CGEventPost(kCGHIDEventTap, scroll);
            CFRelease(scroll);
        }
        char *j = cu_dup_json(alloc, "{\"success\":true}", 16);
        if (!j) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(j, 16);
        return HU_OK;
    }

    if (strcmp(action, "key") == 0) {
        const char *combo = hu_json_get_string(args, "combo");
        if (!combo || !combo[0]) {
            *out = hu_tool_result_fail("missing combo", 13);
            return HU_OK;
        }
        CGEventFlags flags = 0;
        CGKeyCode kc = 0;
        if (!cu_parse_combo(combo, &flags, &kc)) {
            *out = hu_tool_result_fail("invalid combo", 13);
            return HU_OK;
        }
        cu_post_key_combo(kc, flags);
        char *j = cu_dup_json(alloc, "{\"success\":true}", 16);
        if (!j) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(j, 16);
        return HU_OK;
    }

    *out = hu_tool_result_fail("unknown action", 14);
    return HU_OK;

#elif defined(__linux__) && defined(HU_HAS_X11) && HU_HAS_X11

    if (!args || args->type != HU_JSON_OBJECT) {
        *out = hu_tool_result_fail("invalid arguments", 17);
        return HU_OK;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action || !action[0]) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }

    if (strcmp(action, "screenshot") == 0) {
        const char *path = hu_json_get_string(args, "path");
        return cu_linux_screenshot(alloc, c, path, out);
    }

    if (strcmp(action, "click") == 0) {
        double x = hu_json_get_number(args, "x", 0);
        double y = hu_json_get_number(args, "y", 0);
        const char *target = hu_json_get_string(args, "target");
        size_t tlen = target ? strlen(target) : 0;
        if (tlen > 512) {
            *out = hu_tool_result_fail("target too long", 15);
            return HU_OK;
        }
        if (tlen > 0) {
            if (!c->ground_provider) {
                if (x == 0.0 && y == 0.0) {
                    *out = hu_tool_result_fail(
                        "visual grounding requires provider binding or explicit "
                        "coordinates",
                        58);
                    return HU_OK;
                }
            } else {
                char tmpl[] = "/tmp/hu_cu_vgXXXXXX.png";
                int fd = mkstemps(tmpl, 4);
                if (fd < 0) {
                    *out = hu_tool_result_fail("visual grounding temp file failed", 31);
                    return HU_OK;
                }
                (void)close(fd);
                (void)unlink(tmpl);
                if (!cu_linux_try_screenshot_cmd(alloc, c ? c->policy : NULL, tmpl)) {
                    (void)unlink(tmpl);
                    *out = hu_tool_result_fail("visual grounding screenshot failed", 33);
                    return HU_OK;
                }
                double gx = -1.0, gy = -1.0;
                hu_error_t gerr = hu_visual_ground_action(
                    alloc, c->ground_provider, c->ground_model, c->ground_model_len, tmpl,
                    strlen(tmpl), target, tlen, &gx, &gy, NULL, NULL);
                (void)unlink(tmpl);
                if (gerr == HU_OK && gx >= 0.0 && gy >= 0.0) {
                    x = gx;
                    y = gy;
                }
            }
        }
        if (x == 0.0 && y == 0.0) {
            *out = hu_tool_result_fail("visual grounding failed", 22);
            return HU_OK;
        }
        return cu_linux_click(alloc, x, y, out);
    }

    if (strcmp(action, "type") == 0) {
        const char *text = hu_json_get_string(args, "text");
        if (!text) {
            *out = hu_tool_result_fail("missing text", 12);
            return HU_OK;
        }
        size_t tlen = strlen(text);
        if (tlen == 0 || tlen > HU_CU_TEXT_MAX) {
            *out = hu_tool_result_fail("text invalid", 12);
            return HU_OK;
        }
        return cu_linux_type(alloc, c ? c->policy : NULL, text, out);
    }

    if (strcmp(action, "scroll") == 0) {
        double x = hu_json_get_number(args, "x", 0);
        double y = hu_json_get_number(args, "y", 0);
        const char *dir = hu_json_get_string(args, "direction");
        double delta_num = hu_json_get_number(args, "delta", 0);
        int32_t scroll_delta = 0;
        if (delta_num > 0)
            scroll_delta = (int32_t)delta_num;
        else if (delta_num < 0)
            scroll_delta = (int32_t)delta_num;
        else if (dir) {
            if (strcasecmp(dir, "up") == 0)
                scroll_delta = 1;
            else if (strcasecmp(dir, "down") == 0)
                scroll_delta = -1;
        }
        if (scroll_delta == 0)
            scroll_delta = -1;
        return cu_linux_scroll(alloc, x, y, scroll_delta, out);
    }

    if (strcmp(action, "key") == 0) {
        *out = hu_tool_result_fail("key combos require macOS", 24);
        return HU_OK;
    }

    *out = hu_tool_result_fail("unknown action", 14);
    return HU_OK;

#else
    (void)c;
    (void)args;
    *out = hu_tool_result_fail("computer_use requires macOS or Linux with X11/XTest", 52);
    return HU_OK;

#endif
#endif
}

hu_error_t hu_computer_use_screenshot_to_path(hu_allocator_t *alloc, hu_security_policy_t *policy,
                                              const char *path, hu_tool_result_t *out) {
    if (!alloc || !path || !path[0] || !out)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)policy;
    *out = hu_tool_result_fail("computer_use screenshot path unavailable in tests", 44);
    return HU_OK;
#else
    {
        hu_computer_use_ctx_t ctx = {0};
        ctx.policy = policy;
        if (!cu_autonomy_allows(&ctx)) {
            *out = hu_tool_result_fail("computer_use requires supervised autonomy or higher", 49);
            return HU_OK;
        }

#if defined(__APPLE__)
        return cu_mac_screenshot(alloc, &ctx, path, out);
#elif defined(__linux__) && defined(HU_HAS_X11) && HU_HAS_X11
        return cu_linux_screenshot(alloc, &ctx, path, out);
#else
        *out = hu_tool_result_fail("computer_use requires macOS or Linux with X11/XTest", 52);
        return HU_OK;
#endif
    }
#endif
}

static const char *computer_use_name(void *ctx) {
    (void)ctx;
    return HU_CU_NAME;
}
static const char *computer_use_description(void *ctx) {
    (void)ctx;
    return "Control the desktop: macOS (CoreGraphics) or Linux with X11 — screenshot (PNG base64 "
           "or "
           "file), click, type, scroll, key combos (key macOS-only).";
}
static const char *computer_use_parameters_json(void *ctx) {
    (void)ctx;
    return HU_CU_PARAMS;
}

static void computer_use_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(hu_computer_use_ctx_t));
}

static const hu_tool_vtable_t computer_use_vtable = {
    .execute = computer_use_execute,
    .name = computer_use_name,
    .description = computer_use_description,
    .parameters_json = computer_use_parameters_json,
    .deinit = computer_use_deinit,
};

hu_error_t hu_computer_use_create(hu_allocator_t *alloc, hu_security_policy_t *policy,
                                  hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_computer_use_ctx_t *ctx =
        (hu_computer_use_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_computer_use_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->policy = policy;
    *out = (hu_tool_t){.ctx = ctx, .vtable = &computer_use_vtable};
    return HU_OK;
}

void hu_computer_use_set_grounding(hu_tool_t *tool, hu_provider_t *provider, const char *model,
                                   size_t model_len) {
    if (!tool || !tool->ctx)
        return;
    hu_computer_use_ctx_t *c = (hu_computer_use_ctx_t *)tool->ctx;
    c->ground_provider = provider;
    c->ground_model = model;
    c->ground_model_len = model_len;
}
