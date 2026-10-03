/* Hard-moment note (src/agent/hard_moment.c): classify an inbound message as
 * distressed / low mood / neither, and — only when the gate is LIVE — append a
 * short in-voice note to the persona prompt so the reply meets the moment.
 * Low mood is the gap this closes: hu_affect_is_distress needs arousal > 0.5,
 * so "sad", "lonely", "depressed" never registered. */
#include "human/agent.h"
#include "human/agent/hard_moment.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "test_env_guard.h"
#include "test_framework.h"
#include "test_tmpdir.h"
#include "turn_recording_provider.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static hu_hard_moment_kind_t classify(const char *s) {
    return hu_hard_moment_classify(s, s ? strlen(s) : 0);
}

static void hard_moment_stressed_message_is_distress(void) {
    HU_ASSERT_EQ((int)classify("I'm so stressed and scared about tomorrow"),
                 (int)HU_HARD_MOMENT_DISTRESS);
}

static void hard_moment_sad_lonely_message_is_low_mood(void) {
    HU_ASSERT_EQ((int)classify("feeling really sad and lonely tonight"),
                 (int)HU_HARD_MOMENT_LOW_MOOD);
}

static void hard_moment_single_sad_word_is_low_mood(void) {
    /* The exact case the old distress rule missed. */
    HU_ASSERT_EQ((int)classify("honestly just sad today"), (int)HU_HARD_MOMENT_LOW_MOOD);
}

static void hard_moment_positive_and_neutral_are_none(void) {
    HU_ASSERT_EQ((int)classify("had a great day, so happy"), (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_EQ((int)classify("what time is dinner"), (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_EQ((int)classify("tired lol"), (int)HU_HARD_MOMENT_NONE);
}

static void hard_moment_negated_sadness_is_none(void) {
    HU_ASSERT_EQ((int)classify("not sad at all, all good"), (int)HU_HARD_MOMENT_NONE);
}

static void hard_moment_null_and_empty_are_none(void) {
    HU_ASSERT_EQ((int)hu_hard_moment_classify(NULL, 5), (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_EQ((int)hu_hard_moment_classify("", 0), (int)HU_HARD_MOMENT_NONE);
}

static void hard_moment_notes_exist_and_avoid_banned_phrases(void) {
    HU_ASSERT_NULL(hu_hard_moment_note(HU_HARD_MOMENT_NONE));
    const hu_hard_moment_kind_t kinds[] = {HU_HARD_MOMENT_DISTRESS, HU_HARD_MOMENT_LOW_MOOD};
    for (size_t i = 0; i < 2; i++) {
        const char *n = hu_hard_moment_note(kinds[i]);
        HU_ASSERT_NOT_NULL(n);
        /* Persona anti-patterns ban these as AI tells; the note must not
         * plant them in the model's head. */
        HU_ASSERT_NULL(strstr(n, "I'm here for you"));
        HU_ASSERT_NULL(strstr(n, "I understand"));
        HU_ASSERT_NULL(strstr(n, "sorry to hear"));
    }
    HU_ASSERT_TRUE(strcmp(hu_hard_moment_note(HU_HARD_MOMENT_DISTRESS),
                          hu_hard_moment_note(HU_HARD_MOMENT_LOW_MOOD)) != 0);
}

static char *dup_prompt(hu_allocator_t *a, const char *s, size_t *len) {
    *len = strlen(s);
    char *p = (char *)a->alloc(a->ctx, *len + 1);
    memcpy(p, s, *len + 1);
    return p;
}

static void hard_moment_apply_off_leaves_prompt_and_skips_work(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = dup_prompt(&a, "PERSONA", &len);
    const char *msg = "so sad and lonely";
    HU_ASSERT_EQ((int)hu_hard_moment_apply(&a, HU_GATE_OFF, msg, strlen(msg), &p, &len),
                 (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_STR_EQ(p, "PERSONA");
    a.free(a.ctx, p, len + 1);
}

static void hard_moment_apply_shadow_classifies_but_leaves_prompt(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = dup_prompt(&a, "PERSONA", &len);
    const char *msg = "so sad and lonely";
    HU_ASSERT_EQ((int)hu_hard_moment_apply(&a, HU_GATE_SHADOW, msg, strlen(msg), &p, &len),
                 (int)HU_HARD_MOMENT_LOW_MOOD);
    HU_ASSERT_STR_EQ(p, "PERSONA");
    HU_ASSERT_EQ(len, strlen("PERSONA"));
    a.free(a.ctx, p, len + 1);
}

static void hard_moment_apply_live_appends_matching_note(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = dup_prompt(&a, "PERSONA", &len);
    const char *msg = "I'm so stressed and scared about tomorrow";
    HU_ASSERT_EQ((int)hu_hard_moment_apply(&a, HU_GATE_LIVE, msg, strlen(msg), &p, &len),
                 (int)HU_HARD_MOMENT_DISTRESS);
    HU_ASSERT_TRUE(strncmp(p, "PERSONA", 7) == 0);
    HU_ASSERT_NOT_NULL(strstr(p, hu_hard_moment_note(HU_HARD_MOMENT_DISTRESS)));
    HU_ASSERT_EQ(len, strlen(p));
    a.free(a.ctx, p, len + 1);
}

static void hard_moment_apply_live_neutral_message_leaves_prompt(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = dup_prompt(&a, "PERSONA", &len);
    const char *msg = "what time is dinner";
    HU_ASSERT_EQ((int)hu_hard_moment_apply(&a, HU_GATE_LIVE, msg, strlen(msg), &p, &len),
                 (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_STR_EQ(p, "PERSONA");
    a.free(a.ctx, p, len + 1);
}

/* ── DEF-13: the detector runs on hu_agent_turn, the path every reply takes ──
 * Until 2026-10-01 its only call site sat in agent_stream.c after the
 * can_stream=0 return, so production (which never streams) logged zero
 * hard_moment lines and the shadow data the gate's promotion needs did not
 * exist. These drive a real hu_agent_turn with the SAME HU_HARD_MOMENT gate. */

typedef struct {
    hu_agent_t agent;
    trp_t trp;
    char dir[256];
    hu_test_env_guard_t env; /* HOME / HU_STATE_DIR as they were before the turn */
} hm_turn_t;

/* The current test's tmpdir, for the runner to remove (HU_RUN_TEST_ENV_GUARDED
 * restores HOME / HU_STATE_DIR there too, so a failed assert that longjmps
 * past hm_turn_deinit cannot leak them into later suites). */
static char s_hm_scratch[256];

static bool hm_turn_init(hm_turn_t *t, hu_allocator_t *alloc) {
    memset(t, 0, sizeof(*t));
    if (!hu_test_mkdtemp("/tmp/hu_hm_turn_", t->dir, sizeof(t->dir)))
        return false;
    snprintf(s_hm_scratch, sizeof(s_hm_scratch), "%s", t->dir);
    hu_test_env_guard_save(&t->env);
    setenv("HOME", t->dir, 1);
    setenv("HU_STATE_DIR", t->dir, 1);
    trp_init(&t->trp, NULL, 0, "ok.");
    if (hu_agent_from_config(&t->agent, alloc, trp_provider(&t->trp), NULL, 0, NULL, NULL, NULL,
                             NULL, "hm-model", 8, "hm", 2, 0.7, t->dir, strlen(t->dir), 5, 50,
                             false, 1, NULL, 0, NULL, 0, NULL) != HU_OK)
        goto fail_no_agent;
    t->agent.active_channel = "imessage";
    t->agent.active_channel_len = 8;
    hu_persona_t *p = (hu_persona_t *)alloc->alloc(alloc->ctx, sizeof(*p));
    if (!p) {
        hu_agent_deinit(&t->agent);
        goto fail_no_agent;
    }
    memset(p, 0, sizeof(*p));
    p->name = hu_strndup(alloc, "testname", 8);
    p->name_len = 8;
    if (t->agent.persona) {
        hu_persona_deinit(alloc, t->agent.persona);
        alloc->free(alloc->ctx, t->agent.persona, sizeof(hu_persona_t));
    }
    t->agent.persona = p;
    return true;
fail_no_agent:
    trp_deinit(&t->trp);
    hu_test_env_guard_restore(&t->env);
    hu_test_rm_rf(t->dir);
    s_hm_scratch[0] = '\0';
    return false;
}

static void hm_turn_deinit(hm_turn_t *t) {
    hu_agent_deinit(&t->agent);
    trp_deinit(&t->trp);
    hu_test_env_guard_restore(&t->env);
    hu_test_rm_rf(t->dir);
    s_hm_scratch[0] = '\0';
}

/* Run one turn with HU_HARD_MOMENT=<mode>, stderr captured into a malloc'd
 * string (caller frees). The previous HU_HARD_MOMENT value is restored. */
static char *hm_run_turn(hm_turn_t *t, const char *mode, const char *msg) {
    const char *prev = getenv("HU_HARD_MOMENT");
    char prev_copy[32] = {0};
    if (prev)
        snprintf(prev_copy, sizeof(prev_copy), "%s", prev);
    setenv("HU_HARD_MOMENT", mode, 1);

    char path[300];
    snprintf(path, sizeof(path), "%s/stderr.txt", t->dir);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (saved >= 0 && fd >= 0)
        dup2(fd, STDERR_FILENO);

    char *r = NULL;
    size_t rlen = 0;
    hu_error_t err = hu_agent_turn(&t->agent, msg, strlen(msg), &r, &rlen);

    fflush(stderr);
    if (saved >= 0) {
        dup2(saved, STDERR_FILENO);
        close(saved);
    }
    if (fd >= 0)
        close(fd);
    if (prev)
        setenv("HU_HARD_MOMENT", prev_copy, 1);
    else
        unsetenv("HU_HARD_MOMENT");
    if (r)
        t->agent.alloc->free(t->agent.alloc->ctx, r, rlen + 1);
    if (err != HU_OK)
        return NULL;

    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)(n > 0 ? n : 0) + 1);
    size_t got = buf ? fread(buf, 1, (size_t)(n > 0 ? n : 0), f) : 0;
    if (buf)
        buf[got] = '\0';
    fclose(f);
    return buf;
}

/* A question, so the S8 silence gate hands the turn to the model. */
#define HM_MSG          "feeling really sad and lonely tonight, you around?"
#define HM_SHADOW_LINE  "shadow: would add low_mood note"
#define HM_NOTE_SNIPPET "Right now they sound down."

static void hard_moment_agent_turn_shadow_logs_and_leaves_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hm_turn_t t;
    HU_ASSERT_TRUE(hm_turn_init(&t, &alloc));
    char *err_out = hm_run_turn(&t, "shadow", HM_MSG);
    HU_ASSERT_NOT_NULL(err_out);
    HU_ASSERT_NOT_NULL(strstr(err_out, HM_SHADOW_LINE));
    HU_ASSERT_NULL(strstr(err_out, "sad and lonely")); /* never the message text */
    HU_ASSERT_GT(t.trp.calls, 0u); /* the model saw the turn, so the prompt was built */
    HU_ASSERT_NOT_NULL(t.trp.log);
    HU_ASSERT_NULL(strstr(t.trp.log, HM_NOTE_SNIPPET)); /* shadow sends nothing new */
    free(err_out);
    hm_turn_deinit(&t);
}

static void hard_moment_agent_turn_off_is_silent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hm_turn_t t;
    HU_ASSERT_TRUE(hm_turn_init(&t, &alloc));
    char *err_out = hm_run_turn(&t, "off", HM_MSG);
    HU_ASSERT_NOT_NULL(err_out);
    HU_ASSERT_NULL(strstr(err_out, "[hard_moment]"));
    HU_ASSERT_GT(t.trp.calls, 0u);
    HU_ASSERT_NULL(strstr(t.trp.log, HM_NOTE_SNIPPET));
    free(err_out);
    hm_turn_deinit(&t);
}

static void hard_moment_agent_turn_live_puts_the_note_in_the_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hm_turn_t t;
    HU_ASSERT_TRUE(hm_turn_init(&t, &alloc));
    char *err_out = hm_run_turn(&t, "live", HM_MSG);
    HU_ASSERT_NOT_NULL(err_out);
    HU_ASSERT_GT(t.trp.calls, 0u);
    HU_ASSERT_NOT_NULL(t.trp.log);
    HU_ASSERT_NOT_NULL(strstr(t.trp.log, HM_NOTE_SNIPPET));
    free(err_out);
    hm_turn_deinit(&t);
}

void run_hard_moment_tests(void) {
    HU_TEST_SUITE("hard_moment");
    HU_RUN_TEST(hard_moment_stressed_message_is_distress);
    HU_RUN_TEST(hard_moment_sad_lonely_message_is_low_mood);
    HU_RUN_TEST(hard_moment_single_sad_word_is_low_mood);
    HU_RUN_TEST(hard_moment_positive_and_neutral_are_none);
    HU_RUN_TEST(hard_moment_negated_sadness_is_none);
    HU_RUN_TEST(hard_moment_null_and_empty_are_none);
    HU_RUN_TEST(hard_moment_notes_exist_and_avoid_banned_phrases);
    HU_RUN_TEST(hard_moment_apply_off_leaves_prompt_and_skips_work);
    HU_RUN_TEST(hard_moment_apply_shadow_classifies_but_leaves_prompt);
    HU_RUN_TEST(hard_moment_apply_live_appends_matching_note);
    HU_RUN_TEST(hard_moment_apply_live_neutral_message_leaves_prompt);
    HU_RUN_TEST_ENV_GUARDED(hard_moment_agent_turn_shadow_logs_and_leaves_prompt, s_hm_scratch);
    HU_RUN_TEST_ENV_GUARDED(hard_moment_agent_turn_off_is_silent, s_hm_scratch);
    HU_RUN_TEST_ENV_GUARDED(hard_moment_agent_turn_live_puts_the_note_in_the_prompt, s_hm_scratch);
}
