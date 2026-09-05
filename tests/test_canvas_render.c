#include "human/core/allocator.h"
#include "human/tools/canvas_render.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include <string.h>

static void canvas_render_null_args(void) {
    hu_allocator_t a = hu_system_allocator();
    HU_ASSERT_EQ(hu_canvas_render_to_image(NULL, "x", 1, HU_CANVAS_FORMAT_HTML, "/tmp/out.png", 12),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_canvas_render_to_image(&a, NULL, 1, HU_CANVAS_FORMAT_HTML, "/tmp/out.png", 12),
                 HU_ERR_INVALID_ARGUMENT);
}

static void canvas_format_from_string_known(void) {
    HU_ASSERT_EQ(hu_canvas_format_from_string("html", 4), HU_CANVAS_FORMAT_HTML);
    HU_ASSERT_EQ(hu_canvas_format_from_string("svg", 3), HU_CANVAS_FORMAT_SVG);
    HU_ASSERT_EQ(hu_canvas_format_from_string("mermaid", 7), HU_CANVAS_FORMAT_MERMAID);
}

static void canvas_format_from_string_unknown(void) {
    HU_ASSERT_EQ(hu_canvas_format_from_string("xyz", 3), HU_CANVAS_FORMAT_HTML);
}

static void canvas_render_test_mode(void) {
    hu_allocator_t a = hu_system_allocator();
    char out[512];
    HU_ASSERT_TRUE(hu_test_tmppath(out, sizeof(out), "hu_test.png"));
    HU_ASSERT_EQ(
        hu_canvas_render_to_image(&a, "<h1>Hi</h1>", 11, HU_CANVAS_FORMAT_HTML, out, strlen(out)),
        HU_OK);
}

void run_canvas_render_tests(void) {
    HU_TEST_SUITE("CanvasRender");
    HU_RUN_TEST(canvas_render_null_args);
    HU_RUN_TEST(canvas_format_from_string_known);
    HU_RUN_TEST(canvas_format_from_string_unknown);
    HU_RUN_TEST(canvas_render_test_mode);
}
