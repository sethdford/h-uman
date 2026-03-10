/* Terminal capability and color output tests */
#include "human/terminal.h"
#include "test_framework.h"
#include <string.h>

static void test_color_level_enum_values(void) {
    HU_ASSERT_EQ(HU_COLOR_LEVEL_NONE, 0);
    HU_ASSERT_EQ(HU_COLOR_LEVEL_BASIC, 1);
    HU_ASSERT_EQ(HU_COLOR_LEVEL_256, 2);
    HU_ASSERT_EQ(HU_COLOR_LEVEL_TRUECOLOR, 3);
}

static void test_theme_enum_values(void) {
    HU_ASSERT_EQ(HU_THEME_DARK, 0);
    HU_ASSERT_EQ(HU_THEME_LIGHT, 1);
}

static void test_color_level_within_valid_range(void) {
    hu_color_level_t level = hu_terminal_color_level();
    HU_ASSERT_TRUE(level >= HU_COLOR_LEVEL_NONE && level <= HU_COLOR_LEVEL_TRUECOLOR);
}

static void test_color_level_caching(void) {
    hu_color_level_t a = hu_terminal_color_level();
    hu_color_level_t b = hu_terminal_color_level();
    HU_ASSERT_EQ(a, b);
}

static void test_theme_within_valid_range(void) {
    hu_theme_t theme = hu_terminal_theme();
    HU_ASSERT_TRUE(theme == HU_THEME_DARK || theme == HU_THEME_LIGHT);
}

static void test_theme_caching(void) {
    hu_theme_t a = hu_terminal_theme();
    hu_theme_t b = hu_terminal_theme();
    HU_ASSERT_EQ(a, b);
}

static void test_color_fg_returns_buf(void) {
    char buf[32];
    const char *ret = hu_color_fg(buf, 255, 128, 0);
    HU_ASSERT_NOT_NULL(ret);
    HU_ASSERT_TRUE(ret == buf);
}

static void test_color_bg_returns_buf(void) {
    char buf[32];
    const char *ret = hu_color_bg(buf, 0, 128, 255);
    HU_ASSERT_NOT_NULL(ret);
    HU_ASSERT_TRUE(ret == buf);
}

static void test_color_fg_output_format(void) {
    char buf[32];
    hu_color_fg(buf, 100, 100, 100);
    HU_ASSERT_TRUE(buf[0] == '\0' || (buf[0] == '\033' && buf[1] == '['));
}

static void test_color_bg_output_format(void) {
    char buf[32];
    hu_color_bg(buf, 100, 100, 100);
    HU_ASSERT_TRUE(buf[0] == '\0' || (buf[0] == '\033' && buf[1] == '['));
}

static void test_color_buf_null_terminated(void) {
    char buf[32];
    memset(buf, 0xff, sizeof(buf));
    hu_color_fg(buf, 0, 0, 0);
    HU_ASSERT_TRUE(strlen(buf) < sizeof(buf));
}

void run_terminal_tests(void) {
    HU_TEST_SUITE("terminal");
    HU_RUN_TEST(test_color_level_enum_values);
    HU_RUN_TEST(test_theme_enum_values);
    HU_RUN_TEST(test_color_level_within_valid_range);
    HU_RUN_TEST(test_color_level_caching);
    HU_RUN_TEST(test_theme_within_valid_range);
    HU_RUN_TEST(test_theme_caching);
    HU_RUN_TEST(test_color_fg_returns_buf);
    HU_RUN_TEST(test_color_bg_returns_buf);
    HU_RUN_TEST(test_color_fg_output_format);
    HU_RUN_TEST(test_color_bg_output_format);
    HU_RUN_TEST(test_color_buf_null_terminated);
}
