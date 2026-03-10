/* Terminal capability detection and color output */
#ifndef HU_TERMINAL_H
#define HU_TERMINAL_H

#include "design_tokens.h"

typedef enum {
    HU_COLOR_LEVEL_NONE = 0,
    HU_COLOR_LEVEL_BASIC = 1,    /* 16 colors */
    HU_COLOR_LEVEL_256 = 2,      /* 256 colors */
    HU_COLOR_LEVEL_TRUECOLOR = 3 /* 24-bit RGB */
} hu_color_level_t;

typedef enum { HU_THEME_DARK = 0, HU_THEME_LIGHT = 1 } hu_theme_t;

/**
 * Detect terminal color capabilities from environment.
 * Checks NO_COLOR, COLORTERM, TERM, TERM_PROGRAM.
 * Result is cached after first call.
 */
hu_color_level_t hu_terminal_color_level(void);

/**
 * Detect whether the terminal has a light or dark background.
 * Checks COLORFGBG environment variable.
 * Falls back to HU_THEME_DARK.
 */
hu_theme_t hu_terminal_theme(void);

/**
 * Return the appropriate foreground escape for a hex color
 * at the current terminal's capability level.
 * buf must be at least 24 bytes. Returns buf.
 */
const char *hu_color_fg(char *buf, unsigned int r, unsigned int g, unsigned int b);

/**
 * Return the appropriate background escape for a hex color
 * at the current terminal's capability level.
 * buf must be at least 24 bytes. Returns buf.
 */
const char *hu_color_bg(char *buf, unsigned int r, unsigned int g, unsigned int b);

#endif /* HU_TERMINAL_H */
