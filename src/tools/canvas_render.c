#define _DEFAULT_SOURCE
#include "human/tools/canvas_render.h"
#include "human/core/process_util.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#define HU_CANVAS_PATH_MAX 4096

#if defined(HU_IS_TEST) && HU_IS_TEST
static const unsigned char hu_canvas_png_sig[8] = {
    0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au,
};
#endif

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static bool hu_canvas_path_shell_safe(const char *p) {
    if (!p)
        return false;
    if (strstr(p, ".."))
        return false;
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\'' || c == ';' || c == '|' || c == '&' || c == '$' || c == '`' || c == '"' ||
            c == '\n' || c == '\r')
            return false;
    }
    return true;
}
#endif /* !(HU_IS_TEST) */

static hu_error_t hu_canvas_copy_out_path(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    if (src_len == 0 || src_len >= dst_cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return HU_OK;
}

static bool hu_canvas_str_ieq(const char *a, size_t alen, const char *b, size_t blen) {
    size_t i;
    if (alen != blen)
        return false;
    for (i = 0; i < alen; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

hu_canvas_format_t hu_canvas_format_from_string(const char *s, size_t len) {
    if (!s || len == 0)
        return HU_CANVAS_FORMAT_HTML;
    if (hu_canvas_str_ieq(s, len, "html", 4))
        return HU_CANVAS_FORMAT_HTML;
    if (hu_canvas_str_ieq(s, len, "svg", 3))
        return HU_CANVAS_FORMAT_SVG;
    if (hu_canvas_str_ieq(s, len, "mermaid", 7))
        return HU_CANVAS_FORMAT_MERMAID;
    if (hu_canvas_str_ieq(s, len, "markdown", 8))
        return HU_CANVAS_FORMAT_MARKDOWN;
    if (hu_canvas_str_ieq(s, len, "code", 4))
        return HU_CANVAS_FORMAT_CODE;
    if (hu_canvas_str_ieq(s, len, "react", 5))
        return HU_CANVAS_FORMAT_REACT;
    if (hu_canvas_str_ieq(s, len, "mockup", 6))
        return HU_CANVAS_FORMAT_MOCKUP;
    return HU_CANVAS_FORMAT_HTML;
}

#ifndef _WIN32
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static hu_error_t hu_canvas_run_argv(const char *const *argv, int *exit_status) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_run_result_t run = {0};
    hu_error_t err = hu_process_run_with_timeout(&alloc, argv, NULL, 65536, 30, &run);
    if (err != HU_OK)
        return err;
    if (exit_status)
        *exit_status = run.exit_code;
    hu_run_result_free(&alloc, &run);
    return HU_OK;
}

static hu_error_t hu_canvas_write_temp_and_shot(const char *content, size_t content_len,
                                                const char *suffix, const char *out_path,
                                                const char *browser) {
    if (out_path && strchr(out_path, '\''))
        return HU_ERR_INVALID_ARGUMENT;
    char tmpl[64];
    static const char prefix[] = "/tmp/hu_canvas_XXXXXX";
    size_t plen = strlen(prefix);
    size_t slen = strlen(suffix);
    if (plen + slen + 1 > sizeof(tmpl))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(tmpl, prefix, plen);
    memcpy(tmpl + plen, suffix, slen + 1);
    int fd = mkstemps(tmpl, (int)slen);
    if (fd < 0)
        return HU_ERR_IO;
    FILE *wf = fdopen(fd, "wb");
    if (!wf) {
        close(fd);
        unlink(tmpl);
        return HU_ERR_IO;
    }
    if (content_len > 0 && fwrite(content, 1, content_len, wf) != content_len) {
        fclose(wf);
        unlink(tmpl);
        return HU_ERR_IO;
    }
    if (fclose(wf) != 0) {
        unlink(tmpl);
        return HU_ERR_IO;
    }

    char screenshot_arg[HU_CANVAS_PATH_MAX + 16];
    int sa = snprintf(screenshot_arg, sizeof(screenshot_arg), "--screenshot=%s", out_path);
    if (sa < 0 || (size_t)sa >= sizeof(screenshot_arg)) {
        unlink(tmpl);
        return HU_ERR_INVALID_ARGUMENT;
    }
    char file_url[HU_CANVAS_PATH_MAX + 8];
    int fu = snprintf(file_url, sizeof(file_url), "file://%s", tmpl);
    if (fu < 0 || (size_t)fu >= sizeof(file_url)) {
        unlink(tmpl);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *argv[] = {browser, "--headless=new", "--disable-gpu", "--no-sandbox",
                          "--window-size=1280,720", screenshot_arg, file_url, NULL};
    int ex = -1;
    hu_error_t err = hu_canvas_run_argv(argv, &ex);
    unlink(tmpl);
    if (err != HU_OK)
        return err;
    if (ex != 0)
        return HU_ERR_NOT_SUPPORTED;
    return HU_OK;
}

static hu_error_t hu_canvas_chrome_screenshot(const char *content, size_t content_len,
                                              const char *out_path) {
    static const char *browsers[] = {"chromium", "google-chrome", "google-chrome-stable"};
    size_t bi;
    for (bi = 0; bi < sizeof(browsers) / sizeof(browsers[0]); bi++) {
        hu_error_t e = hu_canvas_write_temp_and_shot(content, content_len, ".html", out_path, browsers[bi]);
        if (e == HU_OK)
            return HU_OK;
    }
    return HU_ERR_NOT_SUPPORTED;
}

static hu_error_t hu_canvas_mermaid_render(const char *content, size_t content_len, const char *out_path) {
    if (out_path && strchr(out_path, '\''))
        return HU_ERR_INVALID_ARGUMENT;
    char tmpl[] = "/tmp/hu_canvas_mmd_XXXXXX.mmd";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0)
        return HU_ERR_IO;
    FILE *wf = fdopen(fd, "wb");
    if (!wf) {
        close(fd);
        unlink(tmpl);
        return HU_ERR_IO;
    }
    if (content_len > 0 && fwrite(content, 1, content_len, wf) != content_len) {
        fclose(wf);
        unlink(tmpl);
        return HU_ERR_IO;
    }
    if (fclose(wf) != 0) {
        unlink(tmpl);
        return HU_ERR_IO;
    }

    const char *argv[] = {"mmdc", "-i", tmpl, "-o", out_path, NULL};
    int ex = -1;
    hu_error_t err = hu_canvas_run_argv(argv, &ex);
    unlink(tmpl);
    if (err != HU_OK)
        return err;
    if (ex != 0)
        return HU_ERR_NOT_SUPPORTED;
    return HU_OK;
}
#endif /* !(HU_IS_TEST) */
#endif

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static hu_error_t hu_canvas_write_text_summary(hu_allocator_t *alloc, const char *content, size_t content_len,
                                               const char *label, const char *out_path_stack) {
    FILE *f = fopen(out_path_stack, "wb");
    if (!f)
        return HU_ERR_IO;
    const char *hdr = "Canvas export (";
    if (fputs(hdr, f) < 0 || fputs(label, f) < 0 || fputs(")\nContent length: ", f) < 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    char nb[32];
    snprintf(nb, sizeof(nb), "%zu", content_len);
    if (fputs(nb, f) < 0 || fputs("\n---\n", f) < 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    size_t preview = content_len > 512 ? 512 : content_len;
    if (preview > 0 && fwrite(content, 1, preview, f) != preview) {
        fclose(f);
        return HU_ERR_IO;
    }
    if (fputc('\n', f) == EOF) {
        fclose(f);
        return HU_ERR_IO;
    }
    if (fclose(f) != 0)
        return HU_ERR_IO;
    (void)alloc;
    return HU_OK;
}
#endif /* !(HU_IS_TEST) */

hu_error_t hu_canvas_render_to_image(hu_allocator_t *alloc, const char *content, size_t content_len,
                                     hu_canvas_format_t format, const char *out_path, size_t out_path_len) {
    char path_buf[HU_CANVAS_PATH_MAX];
    hu_error_t err;

    if (!alloc || !content || !out_path)
        return HU_ERR_INVALID_ARGUMENT;
    if (content_len == 0 || out_path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    err = hu_canvas_copy_out_path(path_buf, sizeof(path_buf), out_path, out_path_len);
    if (err != HU_OK)
        return err;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc;
    (void)format;
    FILE *f = fopen(path_buf, "wb");
    if (!f)
        return HU_ERR_IO;
    if (fwrite(hu_canvas_png_sig, 1, sizeof(hu_canvas_png_sig), f) != sizeof(hu_canvas_png_sig)) {
        fclose(f);
        return HU_ERR_IO;
    }
    if (fclose(f) != 0)
        return HU_ERR_IO;
    return HU_OK;
#else
    if (!hu_canvas_path_shell_safe(path_buf))
        return HU_ERR_INVALID_ARGUMENT;

    switch (format) {
    case HU_CANVAS_FORMAT_MARKDOWN:
        return hu_canvas_write_text_summary(alloc, content, content_len, "markdown", path_buf);
    case HU_CANVAS_FORMAT_CODE:
        return hu_canvas_write_text_summary(alloc, content, content_len, "code", path_buf);
#ifndef _WIN32
    case HU_CANVAS_FORMAT_MERMAID:
        return hu_canvas_mermaid_render(content, content_len, path_buf);
    case HU_CANVAS_FORMAT_HTML:
    case HU_CANVAS_FORMAT_SVG: {
        const char *suf = (format == HU_CANVAS_FORMAT_SVG) ? ".svg" : ".html";
        static const char *browsers[] = {"chromium", "google-chrome", "google-chrome-stable"};
        size_t bi;
        for (bi = 0; bi < sizeof(browsers) / sizeof(browsers[0]); bi++) {
            hu_error_t e =
                hu_canvas_write_temp_and_shot(content, content_len, suf, path_buf, browsers[bi]);
            if (e == HU_OK)
                return HU_OK;
        }
        return HU_ERR_NOT_SUPPORTED;
    }
    case HU_CANVAS_FORMAT_REACT:
    case HU_CANVAS_FORMAT_MOCKUP:
        return hu_canvas_chrome_screenshot(content, content_len, path_buf);
#else
    case HU_CANVAS_FORMAT_MERMAID:
    case HU_CANVAS_FORMAT_HTML:
    case HU_CANVAS_FORMAT_SVG:
    case HU_CANVAS_FORMAT_REACT:
    case HU_CANVAS_FORMAT_MOCKUP:
        return HU_ERR_NOT_SUPPORTED;
#endif
    }
    return HU_ERR_NOT_SUPPORTED;
#endif
}
