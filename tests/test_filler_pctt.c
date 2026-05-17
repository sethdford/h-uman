/* PCTT Task 8 — echo-regression eval.
 *
 * Drives 20+ incoming messages through the persona-driven filler selector
 * with a fixed synthetic 5-entry bank that contains NONE of the incoming
 * strings (except one deliberate collision). Asserts:
 *
 *   AC-7: emitted filler is NEVER byte-identical to the incoming message
 *         on triggering channels (the original failure mode preserved in
 *         data/eval_blinded_ab.json:300).
 *   AC-1: iMessage entries (text_fast class) DO trigger when the heuristic
 *         fires, but with a fast 400–1500 ms delay instead of the TEXT_ASYNC
 *         30–60 s delay. Superseded 2026-05-17 from the original "never
 *         trigger" rule — the "speed wins, delays read as dispreference"
 *         intuition lost out to the explicit humanness directive (humans
 *         routinely send "hm" / "lemme think" before substantive replies on
 *         iMessage). The fast delay is what makes the pause read as
 *         thinking rather than disengagement.
 *
 * The corpus is the source-of-truth at data/eval_pctt.json. It is also
 * embedded here so the test runs without a file dependency. If the two
 * drift, the JSON wins; update the embedded copy to match.
 */

#ifdef HU_ENABLE_PERSONA

#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/filler_recency.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HU_TEST_DATA_DIR
#error "HU_TEST_DATA_DIR must be defined when building human_tests"
#endif

#define PCTT_BANK_SIZE 5

/* Synthetic 5-entry bank shared across the test. Chosen so NONE of the
 * corpus's incoming messages match (one deliberate collision is at
 * corpus index 13 — see test_pctt_bank_collision_still_avoids_echo). */
static const char *g_filler_bank[PCTT_BANK_SIZE] = {
    "hmm let me think",      "one sec",        "oh that's interesting",
    "okay give me a moment", "thinking on it",
};

typedef struct {
    const char *incoming;
    const char *channel;
} pctt_entry_t;

/* Mirror of data/eval_pctt.json::corpus. */
static const pctt_entry_t g_corpus[] = {
    {"ooh that's a tough one", "slack"},
    {"What's your take on splitting the inventory schema vs keeping it flat? "
     "Curious if you've seen Postgres patterns that handle both reads and "
     "bulk writes well at this scale.",
     "slack"},
    {"How do I think about staffing the data team given the FY26 freeze?", "slack"},
    {"k", "slack"},
    {"thanks", "slack"},
    {"Is it worth migrating to Bun or sticking with Node 22 LTS?", "discord"},
    {"Why did the build go red on the Thursday push?", "discord"},
    {"ok cool", "discord"},
    {"What's our take on monorepo vs polyrepo for the new platform team — "
     "given how much infra glue we already share between the two existing repos?",
     "slack"},
    {"Can you remind me of the AGI Frontiers timeline?", "slack"},
    {"ooh that's a tough one", "imessage"},
    {"What's the strategy for FY27 if the data team headcount stays flat?", "imessage"},
    {"Why is auth so slow today?", "imessage"},
    {"hmm let me think", "slack"}, /* bank collision */
    {"What if we just kept all three layers and let the orchestrator decide "
     "which to invoke per request type?",
     "discord"},
    {"got it", "slack"},
    {"Have you ever thought about why texting back fast vs slow signals "
     "something completely different on text vs voice?",
     "slack"},
    {"Yeah I'd be down — what time?", "imessage"},
    {"I dunno man, what do you think we should do about the deploy cadence?", "slack"},
    {"What does the eval say about persona tone vs lexical fidelity?", "discord"},
    {"Could you walk me through how the LRU eviction in the recency module "
     "decides which chat to drop?",
     "slack"},
    {"lol", "discord"},
};
#define PCTT_CORPUS_SIZE ((size_t)(sizeof(g_corpus) / sizeof(g_corpus[0])))

/* Build a single-overlay persona pointing at `channel` and the shared bank.
 * Strings are pointers into the test's static storage — caller must NOT
 * destroy/free them. */
static void build_persona(hu_persona_t *p, hu_persona_overlay_t *ov, const char *channel) {
    memset(p, 0, sizeof(*p));
    memset(ov, 0, sizeof(*ov));
    ov->channel = (char *)channel;
    /* filler_bank is `char **`; cast away const from g_filler_bank.
     * The selector treats the bank as read-only. */
    ov->filler_bank = (char **)(void *)g_filler_bank;
    ov->filler_bank_count = PCTT_BANK_SIZE;
    ov->filler_bank_cap = PCTT_BANK_SIZE;
    p->overlays = ov;
    p->overlays_count = 1;
}

static void test_pctt_no_echo_across_corpus(void) {
    hu_filler_recency_t recency = {0};
    size_t triggered_count = 0;
    for (size_t i = 0; i < PCTT_CORPUS_SIZE; i++) {
        const pctt_entry_t *e = &g_corpus[i];
        hu_persona_t p;
        hu_persona_overlay_t ov;
        build_persona(&p, &ov, e->channel);

        char chat_id[64];
        int written = snprintf(chat_id, sizeof(chat_id), "pctt-chat-%zu", i);
        HU_ASSERT(written > 0 && (size_t)written < sizeof(chat_id));

        hu_thinking_context_t ctx = {0};
        ctx.persona = &p;
        ctx.channel_name = e->channel;
        ctx.chat_id = chat_id;
        ctx.chat_id_len = (size_t)written;
        ctx.recency = &recency;
        ctx.seed = (uint32_t)(0xC0FFEE + i);

        hu_thinking_response_t out = {0};
        bool ok = hu_conversation_classify_thinking(&ctx, e->incoming, strlen(e->incoming), NULL, 0,
                                                    &out);
        if (!ok)
            continue;
        triggered_count++;

        /* AC-7: emitted filler must NEVER byte-equal the incoming message. */
        HU_ASSERT(out.filler_len > 0);
        if (out.filler_len == strlen(e->incoming) &&
            memcmp(out.filler, e->incoming, out.filler_len) == 0) {
            fprintf(stderr, "PCTT echo regression at corpus[%zu]: incoming==filler==\"%s\"\n", i,
                    e->incoming);
            HU_ASSERT(0);
        }

        /* Sanity: emitted filler must be one of the bank entries. */
        bool from_bank = false;
        for (size_t b = 0; b < PCTT_BANK_SIZE; b++) {
            if (out.filler_len == strlen(g_filler_bank[b]) &&
                memcmp(out.filler, g_filler_bank[b], out.filler_len) == 0) {
                from_bank = true;
                break;
            }
        }
        HU_ASSERT(from_bank);
    }
    /* The corpus is engineered so a meaningful number trigger; if zero did,
     * the trigger heuristic regressed. */
    HU_ASSERT(triggered_count >= 5);
}

static void test_pctt_imessage_triggers_with_fast_delay(void) {
    /* AC-1 (revised 2026-05-17): text_fast channels DO trigger fillers when
     * the heuristic fires; when they do, the delay must be in the fast
     * 400–1500 ms band (not TEXT_ASYNC's 30–60 s band, which would feel
     * broken on iMessage). Short / casual messages still don't trigger —
     * the heuristic floor (msg_len > 35 AND words > 6 AND ?, or msg_len > 80
     * AND words > 10) keeps casual texting from over-firing.
     *
     * Per-entry expectations (computed against the corpus, not hardcoded):
     *   short / no-question / no-floor-clear  → must NOT trigger
     *   long substantive question on iMessage → MUST trigger, fast delay
     *
     * We don't duplicate the heuristic in the test; instead we run the
     * classifier and verify that (a) at least one iMessage entry triggers
     * (corpus contains a long question), (b) every triggering iMessage
     * call has a fast delay. Pin: if iMessage ever gets coupled to the
     * 30s TEXT_ASYNC delay again, this fails loudly. */
    hu_filler_recency_t recency = {0};
    size_t imessage_seen = 0;
    size_t imessage_triggers = 0;
    for (size_t i = 0; i < PCTT_CORPUS_SIZE; i++) {
        const pctt_entry_t *e = &g_corpus[i];
        if (strcmp(e->channel, "imessage") != 0)
            continue;
        imessage_seen++;

        hu_persona_t p;
        hu_persona_overlay_t ov;
        build_persona(&p, &ov, e->channel);

        hu_thinking_context_t ctx = {0};
        ctx.persona = &p;
        ctx.channel_name = e->channel;
        ctx.chat_id = "imessage-chat";
        ctx.chat_id_len = strlen("imessage-chat");
        ctx.recency = &recency;
        ctx.seed = (uint32_t)i;

        hu_thinking_response_t out = {0};
        bool ok = hu_conversation_classify_thinking(&ctx, e->incoming, strlen(e->incoming), NULL, 0,
                                                    &out);
        if (ok) {
            imessage_triggers++;
            /* When iMessage triggers, the contract is: filler from bank +
             * fast 400-1500 ms delay. If either invariant fails, the new
             * AC-1 was implemented incorrectly. */
            HU_ASSERT(out.filler_len > 0);
            HU_ASSERT(out.delay_ms >= 400);
            HU_ASSERT(out.delay_ms <= 1500);
        }
    }
    HU_ASSERT(imessage_seen >= 3); /* corpus must keep at least 3 iMessage entries */
    /* Regression guard: if a future change silently re-disables iMessage filler
     * (e.g. reintroduces the TEXT_FAST early-return), imessage_triggers drops
     * to 0 and this fires. The corpus contains a long substantive question
     * on iMessage specifically so this assertion holds. */
    HU_ASSERT(imessage_triggers >= 1);
}

static void test_pctt_bank_rotation_under_recency(void) {
    /* Drive the same chat_id with a long triggering question 50 times
     * and verify the selector exercises ≥3 distinct bank entries. With
     * recency tracking forcing rotation away from the previous pick,
     * a healthy uniform-over-(n-1) selector should hit most entries. */
    const char *incoming =
        "What's our take on monorepo vs polyrepo for the new platform team — "
        "given how much infra glue we already share between the two existing repos?";
    hu_filler_recency_t recency = {0};
    hu_persona_t p;
    hu_persona_overlay_t ov;
    build_persona(&p, &ov, "slack");

    bool seen[PCTT_BANK_SIZE];
    memset(seen, 0, sizeof(seen));

    for (uint32_t seed = 1; seed <= 50; seed++) {
        hu_thinking_context_t ctx = {0};
        ctx.persona = &p;
        ctx.channel_name = "slack";
        ctx.chat_id = "rotation-test";
        ctx.chat_id_len = strlen("rotation-test");
        ctx.recency = &recency;
        ctx.seed = seed * 0x9E3779B1u;

        hu_thinking_response_t out = {0};
        bool ok =
            hu_conversation_classify_thinking(&ctx, incoming, strlen(incoming), NULL, 0, &out);
        if (!ok)
            continue;
        for (size_t b = 0; b < PCTT_BANK_SIZE; b++) {
            if (out.filler_len == strlen(g_filler_bank[b]) &&
                memcmp(out.filler, g_filler_bank[b], out.filler_len) == 0) {
                seen[b] = true;
                break;
            }
        }
    }
    size_t hits = 0;
    for (size_t b = 0; b < PCTT_BANK_SIZE; b++)
        if (seen[b])
            hits++;
    HU_ASSERT(hits >= 3); /* should hit at least 3 of 5 over 50 trials */
}

/* Drift guard: verify that g_corpus[] matches data/eval_pctt.json::corpus.
 *
 * The JSON file is the source of truth (see note at top of file). If a
 * developer edits the JSON without updating the embedded C array (or vice
 * versa), this test fires immediately and identifies the divergent index. */
static void test_pctt_corpus_matches_json_fixture(void) {
    const char *path = HU_TEST_DATA_DIR "/eval_pctt.json";
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "PCTT drift guard: cannot open %s\n", path);
        HU_ASSERT(0);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        HU_ASSERT(0);
        return;
    }
    long sz = ftell(f);
    HU_ASSERT(sz > 0 && sz <= (long)(4 * 1024 * 1024));
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    HU_ASSERT_NOT_NULL(buf);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, buf, rd, &root), HU_OK);
    free(buf);
    HU_ASSERT_NOT_NULL(root);
    HU_ASSERT_EQ(root->type, HU_JSON_OBJECT);

    hu_json_value_t *arr = hu_json_object_get(root, "corpus");
    HU_ASSERT_NOT_NULL(arr);
    HU_ASSERT_EQ(arr->type, HU_JSON_ARRAY);

    /* Length must match. */
    if (arr->data.array.len != PCTT_CORPUS_SIZE) {
        fprintf(stderr, "PCTT drift: JSON corpus has %zu entries but g_corpus has %zu\n",
                arr->data.array.len, PCTT_CORPUS_SIZE);
        hu_json_free(&alloc, root);
        HU_ASSERT(0);
        return;
    }

    /* Entry-by-entry comparison. */
    for (size_t i = 0; i < PCTT_CORPUS_SIZE; i++) {
        hu_json_value_t *entry = arr->data.array.items[i];
        HU_ASSERT_NOT_NULL(entry);
        const char *json_in = hu_json_get_string(entry, "incoming");
        const char *json_ch = hu_json_get_string(entry, "channel");
        HU_ASSERT_NOT_NULL(json_in);
        HU_ASSERT_NOT_NULL(json_ch);
        if (strcmp(json_in, g_corpus[i].incoming) != 0 ||
            strcmp(json_ch, g_corpus[i].channel) != 0) {
            fprintf(stderr,
                    "PCTT drift at corpus[%zu]: "
                    "json={\"%s\", \"%s\"}, c={\"%s\", \"%s\"}\n",
                    i, json_in, json_ch, g_corpus[i].incoming, g_corpus[i].channel);
            hu_json_free(&alloc, root);
            HU_ASSERT(0);
            return;
        }
    }

    hu_json_free(&alloc, root);
}

void run_filler_pctt_tests(void) {
    HU_TEST_SUITE("filler_pctt");
    HU_RUN_TEST(test_pctt_no_echo_across_corpus);
    HU_RUN_TEST(test_pctt_imessage_triggers_with_fast_delay);
    HU_RUN_TEST(test_pctt_bank_rotation_under_recency);
    HU_RUN_TEST(test_pctt_corpus_matches_json_fixture);
}

#else /* !HU_ENABLE_PERSONA */

void run_filler_pctt_tests(void) {
    /* persona disabled at build time — nothing to test */
}

#endif
