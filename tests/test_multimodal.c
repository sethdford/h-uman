#include "test_framework.h"
#include "seaclaw/multimodal.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

static void test_base64_encode_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    size_t len = 0;
    sc_error_t err = sc_multimodal_encode_base64(&alloc, "", 0, &out, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(len, 0u);
    SC_ASSERT_NOT_NULL(out);
    alloc.free(alloc.ctx, out, len + 1);
}

static void test_base64_encode_hello(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    size_t len = 0;
    sc_error_t err = sc_multimodal_encode_base64(&alloc, "Hello", 5, &out, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(out, "SGVsbG8=");
    alloc.free(alloc.ctx, out, len + 1);
}

static void test_base64_encode_padding(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    size_t len = 0;
    sc_multimodal_encode_base64(&alloc, "a", 1, &out, &len);
    SC_ASSERT_STR_EQ(out, "YQ==");
    alloc.free(alloc.ctx, out, len + 1);

    sc_multimodal_encode_base64(&alloc, "ab", 2, &out, &len);
    SC_ASSERT_STR_EQ(out, "YWI=");
    alloc.free(alloc.ctx, out, len + 1);

    sc_multimodal_encode_base64(&alloc, "abc", 3, &out, &len);
    SC_ASSERT_STR_EQ(out, "YWJj");
    alloc.free(alloc.ctx, out, len + 1);
}

static void test_detect_mime_png(void) {
    unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    SC_ASSERT_STR_EQ(sc_multimodal_detect_mime(png, 8), "image/png");
}

static void test_detect_mime_jpeg(void) {
    unsigned char jpg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    SC_ASSERT_STR_EQ(sc_multimodal_detect_mime(jpg, 4), "image/jpeg");
}

static void test_detect_mime_unknown(void) {
    unsigned char unk[] = {0x00, 0x01};
    SC_ASSERT_STR_EQ(sc_multimodal_detect_mime(unk, 2), "application/octet-stream");
}

static void test_parse_markers_single(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "Look at [IMAGE:/tmp/photo.png] this image";
    sc_image_ref_t *refs = NULL;
    size_t ref_count = 0;
    char *cleaned = NULL;
    size_t cleaned_len = 0;
    sc_error_t err = sc_multimodal_parse_markers(&alloc, text, strlen(text),
        &refs, &ref_count, &cleaned, &cleaned_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(ref_count, 1u);
    SC_ASSERT_EQ(refs[0].type, SC_IMAGE_REF_LOCAL);
    SC_ASSERT_STR_EQ(refs[0].value, "/tmp/photo.png");
    for (size_t i = 0; i < ref_count; i++)
        alloc.free(alloc.ctx, (void *)refs[i].value, refs[i].value_len + 1);
    alloc.free(alloc.ctx, refs, ref_count * sizeof(sc_image_ref_t));
    alloc.free(alloc.ctx, cleaned, cleaned_len + 1);
}

static void test_parse_markers_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "See [IMG:https://example.com/img.jpg]";
    sc_image_ref_t *refs = NULL;
    size_t ref_count = 0;
    char *cleaned = NULL;
    size_t cleaned_len = 0;
    sc_error_t err = sc_multimodal_parse_markers(&alloc, text, strlen(text),
        &refs, &ref_count, &cleaned, &cleaned_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(ref_count, 1u);
    SC_ASSERT_EQ(refs[0].type, SC_IMAGE_REF_URL);
    for (size_t i = 0; i < ref_count; i++)
        alloc.free(alloc.ctx, (void *)refs[i].value, refs[i].value_len + 1);
    alloc.free(alloc.ctx, refs, ref_count * sizeof(sc_image_ref_t));
    alloc.free(alloc.ctx, cleaned, cleaned_len + 1);
}

static void test_encode_image_mock(void) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_allocator_t alloc = sc_system_allocator();
    char *data_uri = NULL;
    size_t len = 0;
    sc_error_t err = sc_multimodal_encode_image(&alloc, "/nonexistent.png", &data_uri, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(data_uri, "data:image/png;base64,iVBORw0KGgo=");
    alloc.free(alloc.ctx, data_uri, len + 1);
#endif
}

static void test_build_openai_image(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *json = NULL;
    size_t json_len = 0;
    sc_error_t err = sc_multimodal_build_openai_image(&alloc,
        "data:image/png;base64,abc123", 28, &json, &json_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(json);
    SC_ASSERT(json_len > 0);
    alloc.free(alloc.ctx, json, json_len + 1);
}

static void test_build_anthropic_image(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *json = NULL;
    size_t json_len = 0;
    sc_error_t err = sc_multimodal_build_anthropic_image(&alloc,
        "image/png", "abc123", 6, &json, &json_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(json);
    alloc.free(alloc.ctx, json, json_len + 1);
}

static void test_build_gemini_image(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *json = NULL;
    size_t json_len = 0;
    sc_error_t err = sc_multimodal_build_gemini_image(&alloc,
        "image/jpeg", "xyz789", 6, &json, &json_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(json);
    alloc.free(alloc.ctx, json, json_len + 1);
}

static void test_encode_image_raw(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *out = NULL;
    sc_error_t err = sc_multimodal_encode_image_raw("Hi", 2, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_STR_EQ(out, "SGk=");
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_detect_mime_webp(void) {
    unsigned char webp[] = {'R','I','F','F',0,0,0,0,'W','E','B','P'};
    SC_ASSERT_STR_EQ(sc_multimodal_detect_mime(webp, 12), "image/webp");
}

static void test_detect_mime_gif(void) {
    unsigned char gif[] = {'G','I','F','8'};
    SC_ASSERT_STR_EQ(sc_multimodal_detect_mime(gif, 4), "image/gif");
}

static void test_detect_mime_bmp(void) {
    unsigned char bmp[] = {'B','M'};
    SC_ASSERT_STR_EQ(sc_multimodal_detect_mime(bmp, 2), "image/bmp");
}

static void test_parse_markers_multiple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "[IMAGE:/tmp/a.png] and [PHOTO:/tmp/b.jpg]";
    sc_image_ref_t *refs = NULL;
    size_t ref_count = 0;
    char *cleaned = NULL;
    size_t cleaned_len = 0;
    sc_error_t err = sc_multimodal_parse_markers(&alloc, text, strlen(text),
        &refs, &ref_count, &cleaned, &cleaned_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(ref_count, 2u);
    SC_ASSERT_STR_EQ(refs[0].value, "/tmp/a.png");
    SC_ASSERT_STR_EQ(refs[1].value, "/tmp/b.jpg");
    for (size_t i = 0; i < ref_count; i++)
        alloc.free(alloc.ctx, (void *)refs[i].value, refs[i].value_len + 1);
    alloc.free(alloc.ctx, refs, ref_count * sizeof(sc_image_ref_t));
    alloc.free(alloc.ctx, cleaned, cleaned_len + 1);
}

static void test_parse_markers_case_insensitive(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "[image:/tmp/x.png][Image:/tmp/y.png][IMAGE:/tmp/z.png]";
    sc_image_ref_t *refs = NULL;
    size_t ref_count = 0;
    char *cleaned = NULL;
    size_t cleaned_len = 0;
    sc_error_t err = sc_multimodal_parse_markers(&alloc, text, strlen(text),
        &refs, &ref_count, &cleaned, &cleaned_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(ref_count, 3u);
    SC_ASSERT_STR_EQ(refs[0].value, "/tmp/x.png");
    SC_ASSERT_STR_EQ(refs[1].value, "/tmp/y.png");
    SC_ASSERT_STR_EQ(refs[2].value, "/tmp/z.png");
    for (size_t i = 0; i < ref_count; i++)
        alloc.free(alloc.ctx, (void *)refs[i].value, refs[i].value_len + 1);
    alloc.free(alloc.ctx, refs, ref_count * sizeof(sc_image_ref_t));
    alloc.free(alloc.ctx, cleaned, cleaned_len + 1);
}

static void test_parse_markers_img_alias(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "[IMG:https://example.com/pic.jpg]";
    sc_image_ref_t *refs = NULL;
    size_t ref_count = 0;
    char *cleaned = NULL;
    size_t cleaned_len = 0;
    sc_error_t err = sc_multimodal_parse_markers(&alloc, text, strlen(text),
        &refs, &ref_count, &cleaned, &cleaned_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(ref_count, 1u);
    SC_ASSERT_EQ(refs[0].type, SC_IMAGE_REF_URL);
    for (size_t i = 0; i < ref_count; i++)
        alloc.free(alloc.ctx, (void *)refs[i].value, refs[i].value_len + 1);
    alloc.free(alloc.ctx, refs, ref_count * sizeof(sc_image_ref_t));
    alloc.free(alloc.ctx, cleaned, cleaned_len + 1);
}

static void test_parse_markers_data_uri(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "[IMAGE:data:image/png;base64,iVBORw0KGgo=]";
    sc_image_ref_t *refs = NULL;
    size_t ref_count = 0;
    char *cleaned = NULL;
    size_t cleaned_len = 0;
    sc_error_t err = sc_multimodal_parse_markers(&alloc, text, strlen(text),
        &refs, &ref_count, &cleaned, &cleaned_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(ref_count, 1u);
    SC_ASSERT_EQ(refs[0].type, SC_IMAGE_REF_DATA_URI);
    for (size_t i = 0; i < ref_count; i++)
        alloc.free(alloc.ctx, (void *)refs[i].value, refs[i].value_len + 1);
    alloc.free(alloc.ctx, refs, ref_count * sizeof(sc_image_ref_t));
    alloc.free(alloc.ctx, cleaned, cleaned_len + 1);
}

static void test_parse_markers_none(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "Plain text with [bracket] but no IMAGE:";
    sc_image_ref_t *refs = NULL;
    size_t ref_count = 0;
    char *cleaned = NULL;
    size_t cleaned_len = 0;
    sc_error_t err = sc_multimodal_parse_markers(&alloc, text, strlen(text),
        &refs, &ref_count, &cleaned, &cleaned_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(ref_count, 0u);
    SC_ASSERT_NOT_NULL(cleaned);
    if (refs) alloc.free(alloc.ctx, refs, 4 * sizeof(sc_image_ref_t));
    alloc.free(alloc.ctx, cleaned, cleaned_len + 1);
}

static void test_parse_markers_null_alloc(void) {
    sc_image_ref_t *refs = NULL;
    size_t ref_count = 0;
    char *cleaned = NULL;
    size_t cleaned_len = 0;
    sc_error_t err = sc_multimodal_parse_markers(NULL, "x", 1, &refs, &ref_count, &cleaned, &cleaned_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

void run_multimodal_tests(void) {
    SC_TEST_SUITE("Multimodal");
    SC_RUN_TEST(test_base64_encode_empty);
    SC_RUN_TEST(test_base64_encode_hello);
    SC_RUN_TEST(test_base64_encode_padding);
    SC_RUN_TEST(test_detect_mime_png);
    SC_RUN_TEST(test_detect_mime_jpeg);
    SC_RUN_TEST(test_detect_mime_unknown);
    SC_RUN_TEST(test_parse_markers_single);
    SC_RUN_TEST(test_parse_markers_url);
    SC_RUN_TEST(test_encode_image_mock);
    SC_RUN_TEST(test_build_openai_image);
    SC_RUN_TEST(test_build_anthropic_image);
    SC_RUN_TEST(test_build_gemini_image);
    SC_RUN_TEST(test_encode_image_raw);
    SC_RUN_TEST(test_detect_mime_webp);
    SC_RUN_TEST(test_detect_mime_gif);
    SC_RUN_TEST(test_detect_mime_bmp);
    SC_RUN_TEST(test_parse_markers_multiple);
    SC_RUN_TEST(test_parse_markers_case_insensitive);
    SC_RUN_TEST(test_parse_markers_img_alias);
    SC_RUN_TEST(test_parse_markers_data_uri);
    SC_RUN_TEST(test_parse_markers_none);
    SC_RUN_TEST(test_parse_markers_null_alloc);
}
