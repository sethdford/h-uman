/* src/onboard/step_provider.c
 *
 * Sprint 54 US-C2.3 (Phase 1) — Provider setup onboarding step.
 *
 * Phase 1 scope: vtable + menu classification + state persistence +
 * test-injection. Phase 2 (deferred) adds the real stdin/key prompt
 * and the smoke-check call.
 *
 * State write contract:
 *   - For a valid provider choice, write the canonical name to
 *     state->provider.provider_name BEFORE returning NEXT.
 *   - Set state->provider.provider_smoke_passed = false in Phase 1;
 *     Phase 2 sets it true only after the smoke-check returns PASS.
 */

#include "human/onboard/step_provider.h"

#include "human/auth.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/onboard/state.h"
#include "human/onboard/step.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* ── Pure helpers (testable in isolation) ─────────────────────────── */

hu_onboard_provider_choice_t hu_onboard_provider_classify_byte(char c) {
    switch (c) {
    case '1':
        return HU_PROVIDER_CHOICE_MLX_LOCAL;
    case '2':
        return HU_PROVIDER_CHOICE_ANTHROPIC;
    case '3':
        return HU_PROVIDER_CHOICE_GEMINI;
    case '4':
        return HU_PROVIDER_CHOICE_OPENAI;
    case 'q':
    case 'Q':
        return HU_PROVIDER_CHOICE_QUIT;
    default:
        return HU_PROVIDER_CHOICE_INVALID;
    }
}

const char *hu_onboard_provider_choice_to_name(hu_onboard_provider_choice_t c) {
    switch (c) {
    case HU_PROVIDER_CHOICE_MLX_LOCAL:
        return "mlx_local";
    case HU_PROVIDER_CHOICE_ANTHROPIC:
        return "anthropic";
    case HU_PROVIDER_CHOICE_GEMINI:
        return "gemini";
    case HU_PROVIDER_CHOICE_OPENAI:
        return "openai";
    case HU_PROVIDER_CHOICE_NONE:
    case HU_PROVIDER_CHOICE_QUIT:
    case HU_PROVIDER_CHOICE_INVALID:
    default:
        return "";
    }
}

/* Apply a test-injected choice to state and return the injected result.
 * Pure modulo the state write.
 *
 * Encapsulates the state-persistence contract: write the provider name
 * to state BEFORE returning the result so a crash mid-flow preserves
 * the user's choice (crash-safety pattern from welcome step). */
static hu_onboard_step_result_t apply_injected_input(const hu_onboard_provider_test_input_t *inj,
                                                     hu_onboard_state_t *state) {
    if (inj->choice >= HU_PROVIDER_CHOICE_MLX_LOCAL && inj->choice <= HU_PROVIDER_CHOICE_OPENAI) {
        const char *name = hu_onboard_provider_choice_to_name(inj->choice);
        if (state && name && name[0]) {
            /* Persist BEFORE returning so a crash post-step preserves
             * the user's selection. NUL-terminate explicitly. */
            size_t cap = sizeof(state->provider.provider_name);
            size_t n = strlen(name);
            if (n >= cap)
                n = cap - 1;
            memcpy(state->provider.provider_name, name, n);
            state->provider.provider_name[n] = '\0';
            /* Phase 1: smoke-check isn't wired yet, so we leave
             * provider_smoke_passed = false (its init value). Phase 2
             * will flip this to true after PASS. */
            state->provider.provider_smoke_passed = false;
        }
    }
    return inj->injected_result;
}

hu_onboard_step_result_t hu_onboard_step_provider_run_phase1(hu_onboard_step_t *self,
                                                             hu_onboard_state_t *state) {
    /* Test injection: if user_data is set, treat it as the test input
     * struct. The Phase 1 step doesn't read stdin without injection;
     * Phase 2 adds the real stdin read loop. */
    if (self && self->user_data) {
        const hu_onboard_provider_test_input_t *inj =
            (const hu_onboard_provider_test_input_t *)self->user_data;
        return apply_injected_input(inj, state);
    }

    /* No injection in Phase 1 → cannot proceed without stdin handling
     * that's deferred to Phase 2. REPEAT to signal "step ran but
     * nothing happened" without advancing or aborting. */
    return HU_ONBOARD_REPEAT;
}

/* ── Sprint 55 Phase 3 — API-key prompt for cloud providers ───────── */

/* True iff the choice requires an API key (cloud providers). MLX local
 * runs against a local server with a stable shared secret already in
 * config.json, so it does NOT prompt. */
bool hu_onboard_provider_choice_requires_api_key(hu_onboard_provider_choice_t c) {
    return c == HU_PROVIDER_CHOICE_ANTHROPIC || c == HU_PROVIDER_CHOICE_GEMINI ||
           c == HU_PROVIDER_CHOICE_OPENAI;
}

#if !HU_IS_TEST
/* Read a single line from stdin with echo disabled if stdin is a TTY.
 * Returns 0 on success (non-empty line), -1 on EOF / read error, -2 on
 * empty line. tcgetattr/tcsetattr are best-effort: if stdin isn't a
 * TTY we fall back to plain fgets without echo manipulation (this is
 * the same shape `getpass(3)` historically had, minus the deprecation).
 *
 * On return, *out_len is the length of the trimmed line in `buf`
 * (excluding the trailing NUL). The buffer is always NUL-terminated
 * even on failure. Gated by !HU_IS_TEST because tests don't exercise
 * the live tty path (the prompt_and_save_api_key short-circuits). */
static int read_secret_line(char *buf, size_t buf_cap, size_t *out_len) {
    if (!buf || buf_cap == 0 || !out_len)
        return -1;
    buf[0] = '\0';
    *out_len = 0;

    int fd = fileno(stdin);
    struct termios old_state = {0};
    bool tty_changed = false;
    if (fd >= 0 && isatty(fd)) {
        if (tcgetattr(fd, &old_state) == 0) {
            struct termios new_state = old_state;
            new_state.c_lflag &= ~(tcflag_t)ECHO;
            /* TCSAFLUSH discards any input buffered before the call,
             * matching getpass semantics — the user can't type the key
             * before we've turned echo off. */
            if (tcsetattr(fd, TCSAFLUSH, &new_state) == 0) {
                tty_changed = true;
            }
        }
    }

    char *got = fgets(buf, (int)buf_cap, stdin);

    if (tty_changed) {
        /* Restore terminal state regardless of fgets outcome. The echoed
         * newline the user pressed got eaten by ECHO=off, so emit one
         * so the next prompt starts on its own line. */
        tcsetattr(fd, TCSAFLUSH, &old_state);
        fputc('\n', stdout);
        fflush(stdout);
    }

    if (!got) {
        buf[0] = '\0';
        return -1;
    }
    /* Strip trailing CR/LF in-place. */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    *out_len = n;
    return (n == 0) ? -2 : 0;
}

/* Securely wipe `buf[0..cap-1]`. The volatile-pointer dance prevents
 * the compiler from eliding the write as "dead store" — a real-world
 * hazard for secret-clearing routines. Apple platforms have
 * memset_s but it's macOS-only; this pattern is portable. Same
 * !HU_IS_TEST gate as read_secret_line — only used inside the live
 * prompt_and_save_api_key branch. */
static void secure_wipe(char *buf, size_t cap) {
    if (!buf || cap == 0)
        return;
    volatile char *vp = (volatile char *)buf;
    for (size_t i = 0; i < cap; i++) {
        vp[i] = 0;
    }
}
#endif /* !HU_IS_TEST */

/* Prompt for and persist an API key for the chosen cloud provider.
 * Returns true on success, false on any failure (the caller still
 * advances the step; the user can re-enter the key via
 * `human auth set <provider> <key>` later).
 *
 * Under HU_IS_TEST the prompt + write are skipped — tests exercise
 * the key path via the user_data injection seam plus the auth.c
 * unit tests' own coverage of hu_auth_set_api_key. */
static bool prompt_and_save_api_key(const char *provider_name) {
    if (!provider_name || !*provider_name)
        return false;

#if HU_IS_TEST
    (void)provider_name;
    return true;
#else
    fprintf(stdout, "\n  Enter your %s API key (input hidden): ", provider_name);
    fflush(stdout);

    char key[1024];
    size_t key_len = 0;
    int rc = read_secret_line(key, sizeof(key), &key_len);
    if (rc == -1) {
        fputs("  No key entered (EOF/error) — re-run to retry, "
              "or run 'human auth set <provider> <key>' later.\n",
              stdout);
        secure_wipe(key, sizeof(key));
        return false;
    }
    if (rc == -2) {
        fputs("  Empty key — skipping; run 'human auth set <provider> <key>' later.\n", stdout);
        secure_wipe(key, sizeof(key));
        return false;
    }

    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_auth_set_api_key(&alloc, provider_name, key);

    /* ALWAYS wipe before returning, even on success — the key MUST
     * NOT linger on the stack frame. */
    secure_wipe(key, sizeof(key));
    (void)key_len;

    if (err != HU_OK) {
        fprintf(stdout,
                "  API key save failed (err=%d). "
                "You can set it later via 'human auth set %s <key>'.\n",
                (int)err, provider_name);
        return false;
    }

    fputs("  API key saved to ~/.human/auth.json (chmod 0600).\n", stdout);
    return true;
#endif
}

/* ── Sprint 55 Phase 2 — real stdin path ──────────────────────────── */

/* Classify the first non-whitespace byte of a stdin line into a choice.
 * Pure function; tested via the step's vtable run() path with
 * fmemopen-redirected stdin. */
static hu_onboard_provider_choice_t classify_input_line(const char *line) {
    if (!line)
        return HU_PROVIDER_CHOICE_INVALID;
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0' || *line == '\n')
        return HU_PROVIDER_CHOICE_INVALID; /* empty line — re-prompt */
    return hu_onboard_provider_classify_byte(*line);
}

/* Persist a valid provider choice to state. Mirrors the test-injection
 * persist path so behavior is identical regardless of whether the
 * choice came from stdin or from user_data injection. */
static void persist_choice(hu_onboard_provider_choice_t choice, hu_onboard_state_t *state) {
    if (choice < HU_PROVIDER_CHOICE_MLX_LOCAL || choice > HU_PROVIDER_CHOICE_OPENAI)
        return;
    if (!state)
        return;
    const char *name = hu_onboard_provider_choice_to_name(choice);
    if (!name || !name[0])
        return;
    size_t cap = sizeof(state->provider.provider_name);
    size_t n = strlen(name);
    if (n >= cap)
        n = cap - 1;
    memcpy(state->provider.provider_name, name, n);
    state->provider.provider_name[n] = '\0';
    /* Phase 2 deferred: provider_smoke_passed flips true only when
     * the smoke-check (US-C3.3 Phase 2) is invoked here. That requires
     * threading (alloc, cfg) through the step vtable, which is a
     * non-trivial API expansion. Phase 3 will add it. Leave false. */
    state->provider.provider_smoke_passed = false;
}

/* Phase 2 step runner — production stdin path. Test path still goes
 * through user_data injection (see step_provider_run vtable wrapper). */
hu_onboard_step_result_t hu_onboard_step_provider_run(hu_onboard_step_t *self,
                                                      hu_onboard_state_t *state) {
    /* Test injection short-circuit (unchanged from Phase 1). */
    if (self && self->user_data) {
        const hu_onboard_provider_test_input_t *inj =
            (const hu_onboard_provider_test_input_t *)self->user_data;
        return apply_injected_input(inj, state);
    }

    /* Render the menu. Display copy is inline; future stories may move
     * to a copy file once we have more than one onboarding step that
     * needs externalized strings. */
    fputs("\n  Choose your AI provider:\n\n", stdout);
    fputs("    1) Local MLX  (on-device, requires Apple Silicon)\n", stdout);
    fputs("    2) Anthropic  (cloud — requires API key)\n", stdout);
    fputs("    3) Gemini     (cloud — requires API key)\n", stdout);
    fputs("    4) OpenAI     (cloud — requires API key)\n", stdout);
    fputs("    q) Quit\n\n  > ", stdout);
    fflush(stdout);

    char line[64];
    if (!fgets(line, sizeof(line), stdin)) {
        /* EOF or read error → clean quit. */
        return HU_ONBOARD_QUIT;
    }

    hu_onboard_provider_choice_t choice = classify_input_line(line);
    switch (choice) {
    case HU_PROVIDER_CHOICE_QUIT:
        return HU_ONBOARD_QUIT;
    case HU_PROVIDER_CHOICE_MLX_LOCAL:
    case HU_PROVIDER_CHOICE_ANTHROPIC:
    case HU_PROVIDER_CHOICE_GEMINI:
    case HU_PROVIDER_CHOICE_OPENAI:
        /* Persist BEFORE prompting for the key so a crash mid-prompt
         * preserves the user's selection (state-persistence-before-
         * return pattern from Sprint 54 US-C2.2). */
        persist_choice(choice, state);

        /* Sprint 55 Phase 3 — cloud providers need an API key. The
         * key is read via termios echo-off (best effort on TTYs),
         * persisted to ~/.human/auth.json via hu_auth_set_api_key,
         * then secure-wiped from the stack buffer. Failures are
         * non-fatal — the user can set the key later via
         * 'human auth set <provider> <key>'. */
        if (hu_onboard_provider_choice_requires_api_key(choice)) {
            const char *pname = hu_onboard_provider_choice_to_name(choice);
            prompt_and_save_api_key(pname);
        }
        return HU_ONBOARD_NEXT;
    case HU_PROVIDER_CHOICE_INVALID:
    case HU_PROVIDER_CHOICE_NONE:
    default:
        fputs("  Invalid choice. Pick 1, 2, 3, 4, or q.\n", stdout);
        return HU_ONBOARD_REPEAT;
    }
}

/* ── vtable wrapper ───────────────────────────────────────────────── */

static hu_onboard_step_result_t step_provider_run(hu_onboard_step_t *self,
                                                  hu_onboard_state_t *state) {
    return hu_onboard_step_provider_run(self, state);
}

hu_onboard_step_t *hu_onboard_step_provider_create(void) {
    static hu_onboard_step_t step = {
        .name = "provider",
        .display_name = "Provider setup",
        .run = step_provider_run,
        .enter = NULL,
        .user_data = NULL,
    };
    return &step;
}
