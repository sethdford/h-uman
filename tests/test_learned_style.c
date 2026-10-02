/* Learned Style Profile — runtime half (src/persona/learned_style.c,
 * src/agent/turn/learned_style_turn.c, the HU_LEARNED_STYLE gate).
 *
 * Contract under test (shared with the Python learner, Part A):
 *   - the inbound-shape rule and its test vectors, byte for byte;
 *   - lookup order contact bucket -> contact overall -> global, found=false
 *     with no file, and malformed/wrong-schema files treated as absent;
 *   - the mtime cache re-stats at most every 60 s;
 *   - the rendered line names only decisive rates (<= 0.2 / >= 0.8);
 *   - OFF and SHADOW leave the prompt byte-identical; LIVE renders the line
 *     and omits EXACTLY the hand-written length rules (a non-length rule in
 *     the same list survives), in the persona head AND the contact profile.
 *
 * Hermetic: fixture JSON in a mkdtemp dir via HU_PERSONA_DIR, fixture persona
 * structs on the stack. Never reads ~/.human, chat.db or the network. */
#include "human/agent.h"
#include "human/agent/learned_style_turn.h"
#include "human/agent/prompt.h"
#include "human/agent/reply_prompt.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include "human/persona/learned_style.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

/* ── fixtures ─────────────────────────────────────────────────────────── */

#define LS_STATS(n, p50, p90, lower, emoji, endp)                                           \
    "{\"n\":" #n ",\"n_eff\":" #n ".0,\"len_p25\":5,\"len_p50\":" #p50 ",\"len_p90\":" #p90 \
    ",\"bubbles_p50\":1.0,\"lower_start_rate\":" #lower ",\"emoji_rate\":" #emoji           \
    ",\"end_punct_rate\":" #endp ",\"latency_p50_s\":null,\"shrunk\":false}"

#define LS_CONTACT_A "+15550001111"
#define LS_CONTACT_B "+15550002222" /* persona contact the learner omitted (n < 5) */
#define LS_CONTACT_C "+15550003333" /* malformed question bucket */

static const char ls_json_v1[] =
    "{\"schema\":\"learned-style/v1\",\"persona\":\"lstest\","
    "\"generated_at\":\"2026-10-02T05:10:00Z\",\"window_days\":180,\"half_life_days\":21,"
    "\"global\":" LS_STATS(
        500, 30, 120, 0.5, 0.1,
        0.5) ","
             "\"contacts\":{"
             "\"" LS_CONTACT_A "\":{\"overall\":" LS_STATS(
                 80, 25, 90, 0.9, 0.5,
                 0.1) ","
                      "\"buckets\":{\"shape:question\":" LS_STATS(
                          20, 12, 40, 0.95, 0.05,
                          0.85) "}},"
                                "\"" LS_CONTACT_C "\":{\"overall\":" LS_STATS(
                                    40, 18, 60, 0.5, 0.5,
                                    0.5) ","
                                         "\"buckets\":{\"shape:question\":{\"n\":\"many\"}}}"
                                         "}}";

/* Same document, contact A's overall p50 moved 25 -> 40. */
static const char ls_json_v2[] =
    "{\"schema\":\"learned-style/v1\",\"persona\":\"lstest\","
    "\"global\":" LS_STATS(500, 30, 120, 0.5, 0.1,
                           0.5) ","
                                "\"contacts\":{"
                                "\"" LS_CONTACT_A "\":{\"overall\":" LS_STATS(
                                    80, 40, 90, 0.9, 0.5, 0.1) ",\"buckets\":{}}"
                                                               "}}";

static char g_dir[256];

static void ls_write(const char *json, time_t mtime) {
    char path[512];
    snprintf(path, sizeof(path), "%s/lstest.learned-style.json", g_dir);
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    fputs(json, f);
    fclose(f);
    if (mtime) {
        struct timeval tv[2] = {{mtime, 0}, {mtime, 0}};
        utimes(path, tv);
    }
}

static time_t g_fake_now;
static time_t ls_fake_clock(void) {
    return g_fake_now;
}

static void ls_setup(void) {
    snprintf(g_dir, sizeof(g_dir), "/tmp/hu_learned_style_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(g_dir));
    setenv("HU_PERSONA_DIR", g_dir, 1);
    hu_learned_style_cache_reset();
    hu_learned_style_set_clock(NULL);
    hu_learned_style_set_persona("lstest", 6);
}

static void ls_teardown(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/lstest.learned-style.json", g_dir);
    unlink(path);
    rmdir(g_dir);
    unsetenv("HU_PERSONA_DIR");
    unsetenv("HU_LEARNED_STYLE");
    unsetenv("HU_PERSONA_HEAD");
    hu_learned_style_set_clock(NULL);
    hu_learned_style_set_persona(NULL, 0);
    hu_learned_style_cache_reset();
}

/* Persona with every hand-written length rule production carries, each next
 * to a non-length sibling that must survive. */
static char *ls_comm_rules[] = {(char *)"Match energy: short messages get short replies.",
                                (char *)"Never use markdown."};
static char *ls_style_rules[] = {(char *)"Default to natural short texts (5-20 words).",
                                 (char *)"Use contractions always."};
static char *ls_notes[] = {(char *)"Be brief and real", (char *)"dry humor"};
static hu_persona_overlay_t ls_overlay;
static hu_contact_profile_t ls_contacts[2];

static void ls_persona(hu_persona_t *p) {
    memset(p, 0, sizeof(*p));
    p->name = (char *)"lstest";
    p->name_len = 6;
    p->identity = (char *)"Test person who texts from a phone.";
    p->core_anchor = (char *)"You are Test Person.";
    p->communication_rules = ls_comm_rules;
    p->communication_rules_count = 2;
    p->style_rules = ls_style_rules;
    p->style_rules_count = 2;
    memset(&ls_overlay, 0, sizeof(ls_overlay));
    ls_overlay.channel = (char *)"imessage";
    ls_overlay.formality = (char *)"casual";
    ls_overlay.avg_length = (char *)"Default 5-15 words";
    ls_overlay.style_notes = ls_notes;
    ls_overlay.style_notes_count = 2;
    p->overlays = &ls_overlay;
    p->overlays_count = 1;
    memset(ls_contacts, 0, sizeof(ls_contacts));
    ls_contacts[0].contact_id = (char *)LS_CONTACT_A;
    ls_contacts[0].name = (char *)"Alex Rivera";
    ls_contacts[0].dynamic =
        (char *)"Easygoing old friend. Keeps texts short, usually 3-8 words. Loves hiking.";
    ls_contacts[1].contact_id = (char *)LS_CONTACT_B;
    p->contacts = ls_contacts;
    p->contacts_count = 2;
}

/* ── shape rule (vectors shared verbatim with Part A) ───────────────────── */

static void shape_rule_matches_shared_vectors(void) {
    static const struct {
        const char *in;
        hu_ls_shape_t want;
    } v[] = {
        {"you coming tonight?", HU_LS_SHAPE_QUESTION},
        {"ok", HU_LS_SHAPE_CASUAL},
        {"", HU_LS_SHAPE_CASUAL},
        {"lol yes", HU_LS_SHAPE_CASUAL},
        {"So today was wild. Work ran late and then the car wouldn't start. Ended up getting a "
         "ride home from Dave.",
         HU_LS_SHAPE_STORY},
        {"Went to the store. Got milk.", HU_LS_SHAPE_CASUAL},
        {"I don't even know where to start with this week honestly, it has been one thing after "
         "another and I'm tired",
         HU_LS_SHAPE_CASUAL},
        {"We closed on the house!!! Keys tomorrow. Movers Friday. I can't believe it's actually "
         "happening, finally.",
         HU_LS_SHAPE_STORY},
        {"what?? no way", HU_LS_SHAPE_QUESTION},
        {"Long day... really long. Talk later.", HU_LS_SHAPE_CASUAL},
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        HU_ASSERT_EQ((int)hu_learned_style_shape(v[i].in, strlen(v[i].in)), (int)v[i].want);
    HU_ASSERT_EQ((int)hu_learned_style_shape(NULL, 0), (int)HU_LS_SHAPE_CASUAL);
    /* Whitespace is trimmed before measuring. */
    HU_ASSERT_EQ((int)hu_learned_style_shape("  ok \n", 6), (int)HU_LS_SHAPE_CASUAL);
}

static void shape_rule_140_byte_boundary(void) {
    char s[160];
    memset(s, 'a', sizeof(s));
    HU_ASSERT_EQ((int)hu_learned_style_shape(s, 140), (int)HU_LS_SHAPE_STORY);
    HU_ASSERT_EQ((int)hu_learned_style_shape(s, 139), (int)HU_LS_SHAPE_CASUAL);
    /* Padding does not count toward the 140. */
    char padded[160];
    memset(padded, ' ', sizeof(padded));
    memcpy(padded + 5, s, 139);
    HU_ASSERT_EQ((int)hu_learned_style_shape(padded, 150), (int)HU_LS_SHAPE_CASUAL);
}

/* ── lookup ───────────────────────────────────────────────────────────── */

static void lookup_falls_back_bucket_then_contact_then_global(void) {
    ls_setup();
    hu_learned_style_t ls;
    /* No file yet: found=false. */
    HU_ASSERT_FALSE(hu_learned_style_lookup(LS_CONTACT_A, 12, HU_LS_SHAPE_QUESTION, &ls));
    HU_ASSERT_FALSE(ls.found);
    hu_learned_style_cache_reset();
    ls_write(ls_json_v1, 0);

    HU_ASSERT_TRUE(hu_learned_style_lookup(LS_CONTACT_A, 12, HU_LS_SHAPE_QUESTION, &ls));
    HU_ASSERT_TRUE(ls.from_bucket);
    HU_ASSERT_TRUE(ls.from_contact);
    HU_ASSERT_EQ(ls.len_p50, 12);
    HU_ASSERT_EQ(ls.n, 20u);

    /* No story bucket -> the contact's overall. */
    HU_ASSERT_TRUE(hu_learned_style_lookup(LS_CONTACT_A, 12, HU_LS_SHAPE_STORY, &ls));
    HU_ASSERT_FALSE(ls.from_bucket);
    HU_ASSERT_TRUE(ls.from_contact);
    HU_ASSERT_EQ(ls.len_p50, 25);
    HU_ASSERT_EQ(ls.len_p90, 90);
    HU_ASSERT_EQ(ls.latency_p50_s, -1);

    /* Contact absent from the file -> global. */
    HU_ASSERT_TRUE(hu_learned_style_lookup(LS_CONTACT_B, 12, HU_LS_SHAPE_CASUAL, &ls));
    HU_ASSERT_FALSE(ls.from_contact);
    HU_ASSERT_EQ(ls.len_p50, 30);
    HU_ASSERT_EQ(ls.n, 500u);

    /* A malformed bucket is skipped, not fatal: the contact's overall answers. */
    HU_ASSERT_TRUE(hu_learned_style_lookup(LS_CONTACT_C, 12, HU_LS_SHAPE_QUESTION, &ls));
    HU_ASSERT_FALSE(ls.from_bucket);
    HU_ASSERT_TRUE(ls.from_contact);
    HU_ASSERT_EQ(ls.len_p50, 18);
    ls_teardown();
}

static void lookup_treats_malformed_and_wrong_schema_as_absent(void) {
    static const char *bad[] = {
        "{not json",
        "[1,2,3]",
        "{\"schema\":\"learned-style/v0\",\"global\":" LS_STATS(500, 30, 120, 0.5, 0.1, 0.5) "}",
        "{\"schema\":\"learned-style/v1\",\"contacts\":{}}", /* no global */
        "{\"schema\":\"learned-style/v1\",\"global\":" LS_STATS(500, 30, 120, 1.5, 0.1, 0.5) "}",
        "{\"schema\":\"learned-style/v1\",\"global\":{\"n\":3}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ls_setup();
        ls_write(bad[i], 0);
        hu_learned_style_t ls;
        ls.found = true;
        HU_ASSERT_FALSE(hu_learned_style_lookup(LS_CONTACT_A, 12, HU_LS_SHAPE_CASUAL, &ls));
        HU_ASSERT_FALSE(ls.found);
        ls_teardown();
    }
    /* No persona configured: nothing to read. */
    ls_setup();
    ls_write(ls_json_v1, 0);
    hu_learned_style_set_persona(NULL, 0);
    hu_learned_style_t ls;
    HU_ASSERT_FALSE(hu_learned_style_lookup(LS_CONTACT_A, 12, HU_LS_SHAPE_CASUAL, &ls));
    ls_teardown();
}

static void lookup_cache_restats_at_most_every_60s(void) {
    ls_setup();
    g_fake_now = 1800000000;
    hu_learned_style_set_clock(ls_fake_clock);
    ls_write(ls_json_v1, 1700000000);
    hu_learned_style_t ls;
    HU_ASSERT_TRUE(hu_learned_style_lookup(LS_CONTACT_A, 12, HU_LS_SHAPE_CASUAL, &ls));
    HU_ASSERT_EQ(ls.len_p50, 25);

    /* The file changes (new mtime) but the 60 s window has not elapsed. */
    ls_write(ls_json_v2, 1700000100);
    g_fake_now += 59;
    HU_ASSERT_TRUE(hu_learned_style_lookup(LS_CONTACT_A, 12, HU_LS_SHAPE_CASUAL, &ls));
    HU_ASSERT_EQ(ls.len_p50, 25);

    /* Window elapsed: re-stat sees the new mtime and reloads. */
    g_fake_now += 2;
    HU_ASSERT_TRUE(hu_learned_style_lookup(LS_CONTACT_A, 12, HU_LS_SHAPE_CASUAL, &ls));
    HU_ASSERT_EQ(ls.len_p50, 40);
    ls_teardown();
}

/* ── render ───────────────────────────────────────────────────────────── */

static void render_line_names_only_decisive_rates(void) {
    hu_learned_style_t ls;
    memset(&ls, 0, sizeof(ls));
    ls.found = true;
    ls.from_contact = true;
    ls.len_p50 = 25;
    ls.len_p90 = 90;
    ls.lower_start_rate = 0.9f;
    ls.emoji_rate = 0.5f; /* not decisive: no clause */
    ls.end_punct_rate = 0.1f;
    char buf[256];
    size_t n =
        hu_learned_style_render_line(&ls, HU_LS_SHAPE_CASUAL, "Alex Rivera", 11, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "How you text Alex: usually about 25 characters, up to about 90; "
                          "lowercase start most of the time; rarely end with punctuation.");
    HU_ASSERT_EQ(n, strlen(buf));

    /* The other side of each threshold, and the boundaries are inclusive. */
    ls.lower_start_rate = 0.2f;
    ls.end_punct_rate = 0.8f;
    ls.emoji_rate = 0.81f;
    hu_learned_style_render_line(&ls, HU_LS_SHAPE_CASUAL, NULL, 0, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "How you text them: usually about 25 characters, up to about 90; "
                          "start with a capital letter most of the time; usually end with "
                          "punctuation; use an emoji most of the time.");

    /* Nothing decisive: lengths only. */
    ls.lower_start_rate = ls.end_punct_rate = ls.emoji_rate = 0.5f;
    hu_learned_style_render_line(&ls, HU_LS_SHAPE_CASUAL, "Sam", 3, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "How you text Sam: usually about 25 characters, up to about 90.");
}

static void render_line_qualifies_shape_only_from_bucket(void) {
    hu_learned_style_t ls;
    memset(&ls, 0, sizeof(ls));
    ls.found = true;
    ls.from_contact = true;
    ls.len_p50 = 12;
    ls.len_p90 = 40;
    ls.lower_start_rate = ls.end_punct_rate = ls.emoji_rate = 0.5f;
    char buf[256];
    hu_learned_style_render_line(&ls, HU_LS_SHAPE_QUESTION, "Alex", 4, buf, sizeof(buf));
    HU_ASSERT_STR_NOT_CONTAINS(buf, "when they");
    ls.from_bucket = true;
    hu_learned_style_render_line(&ls, HU_LS_SHAPE_QUESTION, "Alex", 4, buf, sizeof(buf));
    HU_ASSERT_STR_CONTAINS(buf, "How you text Alex when they ask something: usually about 12");
    hu_learned_style_render_line(&ls, HU_LS_SHAPE_STORY, "Alex", 4, buf, sizeof(buf));
    HU_ASSERT_STR_CONTAINS(buf, "How you text Alex when they tell you something big:");
    /* Not found / tiny buffer: nothing rendered. */
    HU_ASSERT_EQ(hu_learned_style_render_line(&ls, HU_LS_SHAPE_CASUAL, "Alex", 4, buf, 10), 0u);
    ls.found = false;
    HU_ASSERT_EQ(hu_learned_style_render_line(&ls, HU_LS_SHAPE_CASUAL, "Alex", 4, buf, 256), 0u);
}

/* ── length-rule classifier ──────────────────────────────────────────── */

static void length_rule_classifier_vectors(void) {
    static const char *yes[] = {
        "5-15 words",
        "Default 5-15 words. When someone shares something meaningful: 20-60 words",
        "3-10 words. MAX 15 words.",
        "Don't perform empathy. Be brief and real.",
        "short and concise",
        "Respond like you're thumb-typing on a phone. Short, punchy, real.",
        "Keep it to one line",
        "stay under 60 characters",
        "max: 20",
        "Default to natural short texts (5-20 words).",
    };
    static const char *no[] = {
        "Match the energy and depth of what they said. Short message gets short reply.",
        "Match energy: short messages get short replies.",
        "Use contractions always.",
        "Mostly lowercase, rarely ends with a period",
        "Running 15 minutes late is normal",
        "shortcut jokes are fine",
        "dry humor",
        "",
    };
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
        HU_ASSERT_TRUE(hu_learned_style_is_length_rule(yes[i], strlen(yes[i])));
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
        HU_ASSERT_FALSE(hu_learned_style_is_length_rule(no[i], strlen(no[i])));
    HU_ASSERT_FALSE(hu_learned_style_is_length_rule(NULL, 0));
}

static void strip_contact_removes_only_length_sentences(void) {
    static const char in[] =
        "\n--- Contact profile for +1 ---\nName: Alex\n"
        "Dynamic: Easygoing old friend. Keeps texts short, usually 3-8 words. Loves hiking.\n"
        "Pattern: They prefer short texts. Keep yours short too.\n"
        "Pattern: They text in bursts — wait for the full batch.\n"
        "Interests: short films\n";
    static const char want[] = "\n--- Contact profile for +1 ---\nName: Alex\n"
                               "Dynamic: Easygoing old friend. Loves hiking.\n"
                               "Pattern: They text in bursts — wait for the full batch.\n"
                               "Interests: short films\n";
    char out[512];
    size_t out_len = 0;
    size_t removed = hu_learned_style_strip_contact(in, sizeof(in) - 1, out, sizeof(out), &out_len);
    HU_ASSERT_EQ(removed, 2u);
    HU_ASSERT_STR_EQ(out, want);
    HU_ASSERT_EQ(out_len, sizeof(want) - 1);
    /* Count-only mode agrees. */
    HU_ASSERT_EQ(hu_learned_style_strip_contact(in, sizeof(in) - 1, NULL, 0, NULL), 2u);
    /* A Dynamic line that is nothing but a length rule disappears whole. */
    static const char only[] = "Name: A\nDynamic: Keep it brief.\nWarmth: high\n";
    hu_learned_style_strip_contact(only, sizeof(only) - 1, out, sizeof(out), &out_len);
    HU_ASSERT_STR_EQ(out, "Name: A\nWarmth: high\n");
}

/* ── head builders ────────────────────────────────────────────────────── */

static void compact_head_ex_null_opts_is_byte_identical(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    ls_persona(&p);
    char *a = NULL, *b = NULL;
    size_t al = 0, bl = 0;
    HU_ASSERT_EQ(hu_persona_build_prompt_compact_immersive(&alloc, &p, "imessage", 8, &a, &al),
                 HU_OK);
    HU_ASSERT_EQ(
        hu_persona_build_prompt_compact_immersive_ex(&alloc, &p, "imessage", 8, NULL, &b, &bl),
        HU_OK);
    HU_ASSERT_EQ(al, bl);
    HU_ASSERT_TRUE(memcmp(a, b, al) == 0);
    alloc.free(alloc.ctx, a, al + 1);
    alloc.free(alloc.ctx, b, bl + 1);
}

static void compact_head_ex_suppresses_exactly_length_rules(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    ls_persona(&p);
    static const char line[] = "How you text Alex: usually about 25 characters, up to about 90.";
    hu_persona_style_opts_t o = {line, sizeof(line) - 1, true, 0};
    char *h = NULL;
    size_t hl = 0;
    HU_ASSERT_EQ(
        hu_persona_build_prompt_compact_immersive_ex(&alloc, &p, "imessage", 8, &o, &h, &hl),
        HU_OK);
    HU_ASSERT_STR_CONTAINS(h, line);
    HU_ASSERT_STR_NOT_CONTAINS(h, "5-15 words");
    HU_ASSERT_STR_NOT_CONTAINS(h, "Be brief");
    HU_ASSERT_STR_CONTAINS(h, "dry humor");
    HU_ASSERT_STR_CONTAINS(h, "Match energy: short messages");
    HU_ASSERT_STR_CONTAINS(h, "Never use markdown.");
    /* After the channel style block. */
    HU_ASSERT_TRUE(strstr(h, line) > strstr(h, "dry humor"));
    HU_ASSERT_EQ(o.suppressed, 2u); /* avg_length + one style note */
    alloc.free(alloc.ctx, h, hl + 1);
}

static void ls_agent(hu_agent_t *agent, hu_allocator_t *alloc, hu_persona_t *p, bool lean) {
    memset(agent, 0, sizeof(*agent));
    agent->alloc = alloc;
    agent->persona = p;
    agent->lean_prompt = lean;
    agent->active_channel = "imessage";
    agent->active_channel_len = 8;
    agent->memory_session_id = LS_CONTACT_A;
    agent->memory_session_id_len = 12;
    agent->turn_tier = -1;
}

static void lean_head_ex_suppresses_style_rules_and_overlay(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    ls_persona(&p);
    hu_agent_t agent;
    ls_agent(&agent, &alloc, &p, true);
    static const char line[] = "How you text Alex: usually about 25 characters, up to about 90.";
    hu_persona_style_opts_t o = {line, sizeof(line) - 1, true, 0};
    char *plain = NULL, *h = NULL;
    size_t pl = 0, hl = 0;
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head(&agent, "ok", 2, &plain, &pl), HU_OK);
    HU_ASSERT_STR_CONTAINS(plain, "5-20 words");
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head_ex(&agent, "ok", 2, &o, &h, &hl), HU_OK);
    HU_ASSERT_STR_NOT_CONTAINS(h, "5-20 words");
    HU_ASSERT_STR_NOT_CONTAINS(h, "5-15 words");
    HU_ASSERT_STR_NOT_CONTAINS(h, "Be brief");
    HU_ASSERT_STR_CONTAINS(h, "Use contractions always.");
    HU_ASSERT_STR_CONTAINS(h, "dry humor");
    HU_ASSERT_STR_CONTAINS(h, "Match energy: short messages");
    HU_ASSERT_STR_CONTAINS(h, line);
    HU_ASSERT_EQ(o.suppressed, 3u); /* style rule + avg_length + style note */
    alloc.free(alloc.ctx, plain, pl + 1);
    alloc.free(alloc.ctx, h, hl + 1);
}

/* ── the gate, end to end ─────────────────────────────────────────────── */

static char *ls_render(hu_persona_t *p, const char *incoming, size_t *len) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_reply_prompt_request_t req;
    memset(&req, 0, sizeof(req));
    req.persona = p;
    req.channel = "imessage";
    req.contact = LS_CONTACT_A;
    req.incoming = incoming;
    req.incoming_len = strlen(incoming);
    char *out = NULL;
    HU_ASSERT_EQ(hu_reply_prompt_render(&alloc, &req, &out, len), HU_OK);
    return out;
}

static void gate_off_and_shadow_leave_prompt_byte_identical(void) {
    ls_setup();
    ls_write(ls_json_v1, 0);
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    ls_persona(&p);

    unsetenv("HU_LEARNED_STYLE");
    size_t unset_len = 0;
    char *unset = ls_render(&p, "you around later", &unset_len);
    setenv("HU_LEARNED_STYLE", "off", 1);
    size_t off_len = 0;
    char *off = ls_render(&p, "you around later", &off_len);
    setenv("HU_LEARNED_STYLE", "shadow", 1);
    size_t sh_len = 0;
    char *sh = ls_render(&p, "you around later", &sh_len);

    HU_ASSERT_EQ(unset_len, off_len);
    HU_ASSERT_TRUE(memcmp(unset, off, off_len) == 0);
    HU_ASSERT_EQ(sh_len, off_len);
    HU_ASSERT_TRUE(memcmp(sh, off, off_len) == 0);
    HU_ASSERT_STR_CONTAINS(off, "5-15 words");
    HU_ASSERT_STR_CONTAINS(off, "3-8 words");
    HU_ASSERT_STR_NOT_CONTAINS(off, "How you text");

    /* The apply step itself: OFF/SHADOW never touch the head (same pointer,
     * same bytes), and SHADOW still measured what LIVE would do. */
    hu_agent_t agent;
    ls_agent(&agent, &alloc, &p, true);
    char *head = NULL;
    size_t head_len = 0;
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head(&agent, "ok", 2, &head, &head_len), HU_OK);
    char *before = strdup(head);
    char *before_ptr = head;
    hu_learned_style_turn_t t;
    setenv("HU_LEARNED_STYLE", "off", 1);
    hu_agent_learned_style_apply(&agent, "ok", 2, &head, &head_len, &t);
    HU_ASSERT_TRUE(head == before_ptr);
    HU_ASSERT_STR_EQ(head, before);
    HU_ASSERT_FALSE(t.found);
    setenv("HU_LEARNED_STYLE", "shadow", 1);
    hu_agent_learned_style_apply(&agent, "ok", 2, &head, &head_len, &t);
    HU_ASSERT_TRUE(head == before_ptr);
    HU_ASSERT_STR_EQ(head, before);
    HU_ASSERT_TRUE(t.found);
    HU_ASSERT_FALSE(t.live);
    HU_ASSERT_EQ(t.suppressed_rules, 4u); /* 3 head entries + 1 Dynamic sentence */
    HU_ASSERT_TRUE(t.line_bytes > 0);

    free(before);
    alloc.free(alloc.ctx, head, head_len + 1);
    alloc.free(alloc.ctx, unset, unset_len + 1);
    alloc.free(alloc.ctx, off, off_len + 1);
    alloc.free(alloc.ctx, sh, sh_len + 1);
    ls_teardown();
}

static void gate_live_renders_line_and_suppresses_exactly_length_rules(void) {
    ls_setup();
    ls_write(ls_json_v1, 0);
    hu_persona_t p;
    ls_persona(&p);
    setenv("HU_LEARNED_STYLE", "live", 1);
    size_t len = 0;
    char *out = ls_render(&p, "you around later", &len);
    HU_ASSERT_STR_CONTAINS(out, "How you text Alex: usually about 25 characters, up to about 90; "
                                "lowercase start most of the time; rarely end with punctuation.");
    /* Every hand-written length rule is gone… */
    HU_ASSERT_STR_NOT_CONTAINS(out, "5-20 words");
    HU_ASSERT_STR_NOT_CONTAINS(out, "5-15 words");
    HU_ASSERT_STR_NOT_CONTAINS(out, "Be brief");
    HU_ASSERT_STR_NOT_CONTAINS(out, "3-8 words");
    /* …and nothing else is. */
    HU_ASSERT_STR_CONTAINS(out, "Use contractions always.");
    HU_ASSERT_STR_CONTAINS(out, "dry humor");
    HU_ASSERT_STR_CONTAINS(out, "Match energy: short messages get short replies.");
    HU_ASSERT_STR_CONTAINS(out, "Never use markdown.");
    HU_ASSERT_STR_CONTAINS(out, "Easygoing old friend. Loves hiking.");
    hu_allocator_t alloc = hu_system_allocator();
    alloc.free(alloc.ctx, out, len + 1);

    /* A question to the same contact uses the question bucket's numbers. */
    out = ls_render(&p, "you coming tonight?", &len);
    HU_ASSERT_STR_CONTAINS(out, "How you text Alex when they ask something: usually about 12");
    alloc.free(alloc.ctx, out, len + 1);
    ls_teardown();
}

static void gate_live_compact_head_and_ineligible_turns(void) {
    ls_setup();
    ls_write(ls_json_v1, 0);
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    ls_persona(&p);
    hu_agent_t agent;
    ls_agent(&agent, &alloc, &p, false);
    setenv("HU_LEARNED_STYLE", "live", 1);

    /* HU_PERSONA_HEAD=live (compact immersive head): rebuilt with the line. */
    setenv("HU_PERSONA_HEAD", "live", 1);
    char *head = NULL;
    size_t head_len = 0;
    HU_ASSERT_EQ(hu_agent_build_persona_head(&agent, NULL, 0, &head, &head_len), HU_OK);
    HU_ASSERT_STR_CONTAINS(head, "5-15 words");
    hu_learned_style_turn_t t;
    hu_agent_learned_style_apply(&agent, "ok", 2, &head, &head_len, &t);
    HU_ASSERT_TRUE(t.live);
    HU_ASSERT_STR_CONTAINS(head, "How you text Alex:");
    HU_ASSERT_STR_NOT_CONTAINS(head, "5-15 words");
    HU_ASSERT_STR_CONTAINS(head, "dry humor");
    HU_ASSERT_EQ(strlen(head), head_len);
    alloc.free(alloc.ctx, head, head_len + 1);

    /* Full head (HU_PERSONA_HEAD off): LIVE degrades to no change. */
    unsetenv("HU_PERSONA_HEAD");
    HU_ASSERT_EQ(hu_agent_build_persona_head(&agent, NULL, 0, &head, &head_len), HU_OK);
    char *ptr = head;
    hu_agent_learned_style_apply(&agent, "ok", 2, &head, &head_len, &t);
    HU_ASSERT_TRUE(head == ptr);
    HU_ASSERT_FALSE(t.live);
    alloc.free(alloc.ctx, head, head_len + 1);

    /* Not a persona contact (group chat id, stranger): no-op even in LIVE. */
    agent.lean_prompt = true;
    agent.memory_session_id = "chat123456789";
    agent.memory_session_id_len = 13;
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head(&agent, "ok", 2, &head, &head_len), HU_OK);
    ptr = head;
    hu_agent_learned_style_apply(&agent, "ok", 2, &head, &head_len, &t);
    HU_ASSERT_TRUE(head == ptr);
    HU_ASSERT_FALSE(t.found);
    HU_ASSERT_STR_NOT_CONTAINS(head, "How you text");
    alloc.free(alloc.ctx, head, head_len + 1);

    /* A persona contact with no name and no learned row: global stats, "them". */
    agent.memory_session_id = LS_CONTACT_B;
    agent.memory_session_id_len = 12;
    HU_ASSERT_EQ(hu_agent_build_lean_persona_head(&agent, "ok", 2, &head, &head_len), HU_OK);
    hu_agent_learned_style_apply(&agent, "ok", 2, &head, &head_len, &t);
    HU_ASSERT_TRUE(t.live);
    HU_ASSERT_STR_CONTAINS(head, "How you text them: usually about 30 characters, up to about 120");
    alloc.free(alloc.ctx, head, head_len + 1);
    ls_teardown();
}

static void prompt_strips_contact_length_only_when_live_flag_set(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static const char ctx[] =
        "Name: Alex\nDynamic: Easygoing old friend. Keeps texts short, usually 3-8 words. Loves "
        "hiking.\n";
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.persona_prompt = "You are Test.";
    cfg.persona_prompt_len = 13;
    cfg.persona_immersive = true;
    cfg.contact_context = ctx;
    cfg.contact_context_len = sizeof(ctx) - 1;
    char *a = NULL, *b = NULL;
    size_t al = 0, bl = 0;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &a, &al), HU_OK);
    HU_ASSERT_STR_CONTAINS(a, "3-8 words");
    cfg.learned_style_live = true;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &b, &bl), HU_OK);
    HU_ASSERT_STR_NOT_CONTAINS(b, "3-8 words");
    HU_ASSERT_STR_CONTAINS(b, "Dynamic: Easygoing old friend. Loves hiking.\n");
    /* Exactly the removed sentence's bytes. */
    HU_ASSERT_EQ(al - bl, strlen("Keeps texts short, usually 3-8 words. "));
    alloc.free(alloc.ctx, a, al + 1);
    alloc.free(alloc.ctx, b, bl + 1);
}

void run_learned_style_tests(void) {
    HU_TEST_SUITE("learned_style");
    HU_RUN_TEST(shape_rule_matches_shared_vectors);
    HU_RUN_TEST(shape_rule_140_byte_boundary);
    HU_RUN_TEST(lookup_falls_back_bucket_then_contact_then_global);
    HU_RUN_TEST(lookup_treats_malformed_and_wrong_schema_as_absent);
    HU_RUN_TEST(lookup_cache_restats_at_most_every_60s);
    HU_RUN_TEST(render_line_names_only_decisive_rates);
    HU_RUN_TEST(render_line_qualifies_shape_only_from_bucket);
    HU_RUN_TEST(length_rule_classifier_vectors);
    HU_RUN_TEST(strip_contact_removes_only_length_sentences);
    HU_RUN_TEST(compact_head_ex_null_opts_is_byte_identical);
    HU_RUN_TEST(compact_head_ex_suppresses_exactly_length_rules);
    HU_RUN_TEST(lean_head_ex_suppresses_style_rules_and_overlay);
    HU_RUN_TEST(gate_off_and_shadow_leave_prompt_byte_identical);
    HU_RUN_TEST(gate_live_renders_line_and_suppresses_exactly_length_rules);
    HU_RUN_TEST(gate_live_compact_head_and_ineligible_turns);
    HU_RUN_TEST(prompt_strips_contact_length_only_when_live_flag_set);
}
