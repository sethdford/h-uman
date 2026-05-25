/* src/onboard/step_welcome.c
 *
 * Sprint 51 US-C2.2 — Welcome step implementation.
 *
 * Reads docs/copy/onboarding-step1.md at runtime, prints it to stdout,
 * then waits for user input. The copy file is the single source of
 * truth — wording changes don't require recompilation.
 *
 * Input semantics:
 *   Enter or 'n' or "next"  → HU_ONBOARD_NEXT
 *   'q' or "quit"           → HU_ONBOARD_QUIT
 *   anything else           → HU_ONBOARD_REPEAT (re-prompt)
 *
 * Test mode: when step->user_data is non-NULL, it's read as a
 * (hu_onboard_step_result_t *) and the step returns *user_data without
 * touching stdin. This makes the step deterministic in CI.
 */

#include "human/onboard/step_welcome.h"

#include "human/onboard/state.h"
#include "human/onboard/step.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_WELCOME_COPY_DEFAULT_PATH "docs/copy/onboarding-step1.md"
#define HU_WELCOME_COPY_MAX          (16 * 1024)

/* Mutable global so tests can point at a fixture without environment
 * variables. Reset to default by passing NULL. */
static const char *g_copy_path = HU_WELCOME_COPY_DEFAULT_PATH;

const char *hu_onboard_step_welcome_copy_path(void) {
    return g_copy_path;
}

void hu_onboard_step_welcome_set_copy_path(const char *path) {
    g_copy_path = (path && path[0]) ? path : HU_WELCOME_COPY_DEFAULT_PATH;
}

/* Read up to HU_WELCOME_COPY_MAX bytes from `path` into `out`. NUL-terminates.
 * Returns bytes read (excluding NUL), or 0 on any failure. */
static size_t read_copy_file(const char *path, char *out, size_t cap) {
    if (!path || !out || cap < 2)
        return 0;
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return n;
}

/* Pure helper: classify a single line of user input into a step result.
 * Exported via this TU's static linkage only for clarity — the test
 * harness exercises the FULL step.run() path, not this helper directly. */
static hu_onboard_step_result_t classify_input(const char *line) {
    if (!line)
        return HU_ONBOARD_REPEAT;
    /* Trim leading whitespace + a trailing newline. */
    while (*line == ' ' || *line == '\t')
        line++;
    if (line[0] == '\0' || line[0] == '\n')
        return HU_ONBOARD_NEXT;
    if (line[0] == 'n' || line[0] == 'N')
        return HU_ONBOARD_NEXT;
    if (line[0] == 'q' || line[0] == 'Q')
        return HU_ONBOARD_QUIT;
    return HU_ONBOARD_REPEAT;
}

static hu_onboard_step_result_t welcome_run(hu_onboard_step_t *self, hu_onboard_state_t *state) {
    (void)state;

    /* Test injection: if user_data is set, treat it as the result. */
    if (self->user_data) {
        hu_onboard_step_result_t *injected = (hu_onboard_step_result_t *)self->user_data;
        return *injected;
    }

    /* Print the copy file. */
    char buf[HU_WELCOME_COPY_MAX];
    size_t n = read_copy_file(g_copy_path, buf, sizeof(buf));
    if (n == 0) {
        /* Copy file missing or unreadable. Fallback: print a minimal
         * inline message so the user isn't stuck staring at a blank
         * screen. Continue to prompt for input. */
        fprintf(stdout, "Welcome to human. (copy file %s unreadable)\n", g_copy_path);
    } else {
        fputs(buf, stdout);
    }
    fputs("\n[Enter] continue   [q] quit\n> ", stdout);
    fflush(stdout);

    /* Read one line of input. */
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) {
        /* EOF or read error — treat as QUIT to give the user a clean exit. */
        return HU_ONBOARD_QUIT;
    }
    return classify_input(line);
}

hu_onboard_step_t *hu_onboard_step_welcome_create(void) {
    static hu_onboard_step_t step = {
        .name = "welcome",
        .display_name = "Welcome",
        .run = welcome_run,
        .enter = NULL,
        .user_data = NULL,
    };
    return &step;
}
