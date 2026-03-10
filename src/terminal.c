#include "human/terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_color_level_t cached_level = (hu_color_level_t)-1;
static hu_theme_t cached_theme = (hu_theme_t)-1;

static int env_is(const char *name, const char *val) {
    const char *e = getenv(name);
    return e && strcmp(e, val) == 0;
}

static int env_contains(const char *name, const char *needle) {
    const char *e = getenv(name);
    return e && strstr(e, needle) != NULL;
}

hu_color_level_t hu_terminal_color_level(void) {
    if (cached_level != (hu_color_level_t)-1)
        return cached_level;

    if (getenv("NO_COLOR") || env_is("TERM", "dumb")) {
        cached_level = HU_COLOR_LEVEL_NONE;
        return cached_level;
    }

    if (env_is("COLORTERM", "truecolor") || env_is("COLORTERM", "24bit")) {
        cached_level = HU_COLOR_LEVEL_TRUECOLOR;
        return cached_level;
    }

    /* Known truecolor terminals */
    const char *tp = getenv("TERM_PROGRAM");
    if (tp &&
        (strcmp(tp, "iTerm.app") == 0 || strcmp(tp, "WezTerm") == 0 ||
         strcmp(tp, "Alacritty") == 0 || strcmp(tp, "kitty") == 0 ||
         strcmp(tp, "WarpTerminal") == 0 || strcmp(tp, "ghostty") == 0 || strcmp(tp, "rio") == 0)) {
        cached_level = HU_COLOR_LEVEL_TRUECOLOR;
        return cached_level;
    }

    if (env_contains("TERM", "256color")) {
        cached_level = HU_COLOR_LEVEL_256;
        return cached_level;
    }

    if (getenv("COLORTERM")) {
        cached_level = HU_COLOR_LEVEL_256;
        return cached_level;
    }

    cached_level = HU_COLOR_LEVEL_BASIC;
    return cached_level;
}

hu_theme_t hu_terminal_theme(void) {
    if (cached_theme != (hu_theme_t)-1)
        return cached_theme;

    const char *cfg = getenv("HU_THEME");
    if (cfg) {
        if (strcmp(cfg, "light") == 0) {
            cached_theme = HU_THEME_LIGHT;
            return cached_theme;
        }
        cached_theme = HU_THEME_DARK;
        return cached_theme;
    }

    /* COLORFGBG is "fg;bg" — if bg >= 8, it's usually light */
    const char *colorfgbg = getenv("COLORFGBG");
    if (colorfgbg) {
        const char *semi = strrchr(colorfgbg, ';');
        if (semi) {
            int bg = (int)strtol(semi + 1, NULL, 10);
            cached_theme = (bg >= 8) ? HU_THEME_LIGHT : HU_THEME_DARK;
            return cached_theme;
        }
    }

    cached_theme = HU_THEME_DARK;
    return cached_theme;
}

static unsigned int rgb_to_ansi256(unsigned int r, unsigned int g, unsigned int b) {
    if (r == g && g == b) {
        if (r < 8)
            return 16;
        if (r > 248)
            return 231;
        return (unsigned int)(((double)r - 8.0) / 247.0 * 24.0) + 232;
    }
    unsigned int ri = (unsigned int)(((double)r / 255.0) * 5.0 + 0.5);
    unsigned int gi = (unsigned int)(((double)g / 255.0) * 5.0 + 0.5);
    unsigned int bi = (unsigned int)(((double)b / 255.0) * 5.0 + 0.5);
    return 16 + 36 * ri + 6 * gi + bi;
}

static unsigned int rgb_to_ansi16(unsigned int r, unsigned int g, unsigned int b) {
    unsigned int bright = (r > 128 || g > 128 || b > 128) ? 60 : 0;
    unsigned int base = 30;
    if (r > 64)
        base += 1;
    if (g > 64)
        base += 2;
    if (b > 64)
        base += 4;
    return base + bright;
}

const char *hu_color_fg(char *buf, unsigned int r, unsigned int g, unsigned int b) {
    hu_color_level_t level = hu_terminal_color_level();
    switch (level) {
    case HU_COLOR_LEVEL_TRUECOLOR:
        snprintf(buf, 24, "\033[38;2;%u;%u;%um", r, g, b);
        break;
    case HU_COLOR_LEVEL_256:
        snprintf(buf, 24, "\033[38;5;%um", rgb_to_ansi256(r, g, b));
        break;
    case HU_COLOR_LEVEL_BASIC:
        snprintf(buf, 24, "\033[%um", rgb_to_ansi16(r, g, b));
        break;
    default:
        buf[0] = '\0';
        break;
    }
    return buf;
}

const char *hu_color_bg(char *buf, unsigned int r, unsigned int g, unsigned int b) {
    hu_color_level_t level = hu_terminal_color_level();
    switch (level) {
    case HU_COLOR_LEVEL_TRUECOLOR:
        snprintf(buf, 24, "\033[48;2;%u;%u;%um", r, g, b);
        break;
    case HU_COLOR_LEVEL_256:
        snprintf(buf, 24, "\033[48;5;%um", rgb_to_ansi256(r, g, b));
        break;
    case HU_COLOR_LEVEL_BASIC:
        snprintf(buf, 24, "\033[%um", rgb_to_ansi16(r, g, b) + 10);
        break;
    default:
        buf[0] = '\0';
        break;
    }
    return buf;
}
