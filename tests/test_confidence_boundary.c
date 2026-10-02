/* Confidence boundary (HU_CONFIDENCE_BOUNDARY): something contact A told the
 * twin must not reach the prompt of a conversation with contact B.
 * include/human/memory/confidence_boundary.h, src/memory/confidence_*.c. */
#include "test_framework.h"

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/daemon/share_queue.h"
#include "human/memory/confidence_boundary.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_a[] = "+15550000001"; /* confides */
static const char k_b[] = "+15550000002"; /* is texting now */
#define A_LEN (sizeof(k_a) - 1)
#define B_LEN (sizeof(k_b) - 1)

/* ── pure rules ─────────────────────────────────────────────────────────── */

static void derive_row_rules_first_match_wins(void) {
    char c[HU_CB_CONTACT_MAX];
    HU_ASSERT_EQ(
        hu_confidence_derive_row("_pref:coffee", 12, NULL, 0, NULL, 0, k_a, A_LEN, c, sizeof(c)),
        HU_SHARE_OWNER_SELF);
    HU_ASSERT_EQ(hu_confidence_derive_row("k", 1, k_a, A_LEN, NULL, 0, NULL, 0, c, sizeof(c)),
                 HU_SHARE_PRIVATE_TO_SOURCE);
    HU_ASSERT_STR_EQ(c, k_a);
    const char promise[] = "agent-promise:+15550000002:1700000000";
    HU_ASSERT_EQ(
        hu_confidence_derive_row(promise, strlen(promise), NULL, 0, NULL, 0, NULL, 0, c, sizeof(c)),
        HU_SHARE_PRIVATE_TO_SOURCE);
    HU_ASSERT_STR_EQ(c, k_b);
    HU_ASSERT_EQ(
        hu_confidence_derive_row("_ep:+15550000001", 16, NULL, 0, NULL, 0, NULL, 0, c, sizeof(c)),
        HU_SHARE_PRIVATE_TO_SOURCE);
    HU_ASSERT_STR_EQ(c, k_a);
    HU_ASSERT_EQ(
        hu_confidence_derive_row("experience:x", 12, NULL, 0, NULL, 0, k_b, B_LEN, c, sizeof(c)),
        HU_SHARE_PRIVATE_TO_SOURCE);
    HU_ASSERT_STR_EQ(c, k_b);
    HU_ASSERT_EQ(hu_confidence_derive_row("note:x", 6, NULL, 0, "cli", 3, NULL, 0, c, sizeof(c)),
                 HU_SHARE_OWNER_SELF);
    /* unknown source: conservative private, source unknown */
    HU_ASSERT_EQ(
        hu_confidence_derive_row("_ep:global", 10, NULL, 0, NULL, 0, NULL, 0, c, sizeof(c)),
        HU_SHARE_PRIVATE_TO_SOURCE);
    HU_ASSERT_STR_EQ(c, "");
}

static void excludes_only_private_items_of_someone_else(void) {
    HU_ASSERT_TRUE(hu_confidence_excludes(HU_SHARE_PRIVATE_TO_SOURCE, k_a, A_LEN, k_b, B_LEN));
    HU_ASSERT_FALSE(hu_confidence_excludes(HU_SHARE_PRIVATE_TO_SOURCE, k_b, B_LEN, k_b, B_LEN));
    HU_ASSERT_TRUE(hu_confidence_excludes(HU_SHARE_PRIVATE_TO_SOURCE, "", 0, k_b, B_LEN));
    HU_ASSERT_TRUE(hu_confidence_excludes(HU_SHARE_UNSET, "", 0, k_b, B_LEN));
    HU_ASSERT_FALSE(hu_confidence_excludes(HU_SHARE_OWNER_SELF, "", 0, k_b, B_LEN));
    HU_ASSERT_FALSE(hu_confidence_excludes(HU_SHARE_SHAREABLE, k_a, A_LEN, k_b, B_LEN));
    /* a prefix of the current contact is another contact */
    HU_ASSERT_TRUE(hu_confidence_excludes(HU_SHARE_PRIVATE_TO_SOURCE, k_b, B_LEN - 1, k_b, B_LEN));
    /* no current contact (owner CLI): nothing is anyone else's */
    HU_ASSERT_FALSE(hu_confidence_excludes(HU_SHARE_PRIVATE_TO_SOURCE, k_a, A_LEN, NULL, 0));
}

static void derive_fact_uses_handle_then_owner_channel(void) {
    hu_heuristic_fact_t f;
    char c[HU_CB_CONTACT_MAX];
    memset(&f, 0, sizeof(f));
    memcpy(f.provenance.contact_handle, k_a, A_LEN);
    HU_ASSERT_EQ(hu_confidence_derive_fact(&f, c, sizeof(c)), HU_SHARE_PRIVATE_TO_SOURCE);
    HU_ASSERT_STR_EQ(c, k_a);
    memset(&f, 0, sizeof(f));
    memcpy(f.provenance.channel, "cli", 3);
    HU_ASSERT_EQ(hu_confidence_derive_fact(&f, c, sizeof(c)), HU_SHARE_OWNER_SELF);
    memset(&f, 0, sizeof(f));
    memcpy(f.provenance.channel, "imessage", 8); /* an inbound with no handle */
    HU_ASSERT_EQ(hu_confidence_derive_fact(&f, c, sizeof(c)), HU_SHARE_PRIVATE_TO_SOURCE);
    HU_ASSERT_STR_EQ(c, "");
}

static void gate_parses_off_shadow_live(void) {
    hu_confidence_set_mode_for_test(-1);
    unsetenv("HU_CONFIDENCE_BOUNDARY");
    HU_ASSERT_EQ(hu_confidence_mode(), HU_GATE_OFF);
    setenv("HU_CONFIDENCE_BOUNDARY", "shadow", 1);
    HU_ASSERT_EQ(hu_confidence_mode(), HU_GATE_SHADOW);
    setenv("HU_CONFIDENCE_BOUNDARY", "live", 1);
    HU_ASSERT_EQ(hu_confidence_mode(), HU_GATE_LIVE);
    setenv("HU_CONFIDENCE_BOUNDARY", "bogus", 1);
    HU_ASSERT_EQ(hu_confidence_mode(), HU_GATE_OFF);
    unsetenv("HU_CONFIDENCE_BOUNDARY");
}

/* ── outbound backstop ──────────────────────────────────────────────────── */

static const char k_confided[] = "Task: Priya is pregnant and nobody knows yet";
static const char k_draft[] = "omg haha. Priya is pregnant btw. see you saturday";

static char *dup_str(hu_allocator_t *a, const char *s) {
    size_t n = strlen(s);
    char *p = (char *)a->alloc(a->ctx, n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static size_t backstop_in_mode(int mode, char **out, size_t *out_len) {
    hu_allocator_t a = hu_system_allocator();
    hu_confidence_set_mode_for_test(mode);
    hu_confidence_ledger_clear();
    hu_confidence_ledger_note(k_b, B_LEN, k_confided, strlen(k_confided));
    *out = dup_str(&a, k_draft);
    *out_len = strlen(k_draft);
    size_t n = hu_confidence_backstop_apply(&a, k_b, B_LEN, out, out_len);
    hu_confidence_set_mode_for_test(-1);
    return n;
}

static void backstop_live_drops_the_sentence_naming_the_confidence(void) {
    hu_allocator_t a = hu_system_allocator();
    char *r = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(backstop_in_mode(HU_GATE_LIVE, &r, &len), 1);
    HU_ASSERT_STR_EQ(r, "omg haha. see you saturday");
    HU_ASSERT_EQ(len, strlen(r));
    HU_ASSERT_EQ(hu_confidence_ledger_count(), 0);
    a.free(a.ctx, r, len + 1);
}

static void backstop_shadow_and_off_send_the_draft_unchanged(void) {
    hu_allocator_t a = hu_system_allocator();
    char *r = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(backstop_in_mode(HU_GATE_SHADOW, &r, &len), 1); /* counted */
    HU_ASSERT_STR_EQ(r, k_draft);
    a.free(a.ctx, r, len + 1);
    HU_ASSERT_EQ(backstop_in_mode(HU_GATE_OFF, &r, &len), 0);
    HU_ASSERT_STR_EQ(r, k_draft);
    a.free(a.ctx, r, len + 1);
}

static void backstop_needs_name_and_content_word(void) {
    hu_confidence_ledger_clear();
    hu_confidence_ledger_note(k_b, B_LEN, k_confided, strlen(k_confided));
    char out[128];
    size_t out_len = 0;
    const char hi[] = "Priya says hi";                  /* the name alone */
    const char news[] = "nobody knows what's pregnant"; /* content words, no name */
    HU_ASSERT_EQ(hu_confidence_backstop_scan(hi, strlen(hi), out, &out_len), 0);
    HU_ASSERT_STR_EQ(out, hi);
    HU_ASSERT_EQ(hu_confidence_backstop_scan(news, strlen(news), out, &out_len), 0);
    const char lower[] = "priya is PREGNANT"; /* the reply's casing does not matter */
    HU_ASSERT_EQ(hu_confidence_backstop_scan(lower, strlen(lower), out, &out_len), 1);
    HU_ASSERT_EQ(out_len, 0);
    hu_confidence_ledger_clear();
}

#ifdef HU_ENABLE_SQLITE
#include "human/agent/commitment_store.h"
#include "human/agent/episodic.h"
#include "human/agent/memory_loader.h"
#include "human/experience.h"
#include "human/memory/confidence_repo.h"
#include "human/memory/engines.h"
#include "human/memory/retrieval.h"

/* A fixture DB: A confides during A's conversation (the real experience writer
 * stores it globally), the owner has a preference, B has its own row. */
static hu_memory_t fixture(hu_allocator_t *a) {
    hu_memory_t mem = hu_sqlite_memory_create(a, ":memory:");
    mem.current_session_id = k_a;
    mem.current_session_id_len = A_LEN;
    hu_experience_store_t exp;
    HU_ASSERT_EQ(hu_experience_store_init(a, &mem, &exp), HU_OK);
    exp.db = NULL; /* the semantic-memory write is the path under test */
    const char task[] = "news: Priya is pregnant and nobody knows yet";
    HU_ASSERT_EQ(hu_experience_record(&exp, task, strlen(task), "agent_turn", 10, "wow ok", 6, 1.0),
                 HU_OK);
    hu_experience_store_deinit(&exp);
    mem.current_session_id = NULL;
    mem.current_session_id_len = 0;
    const char pref[] = "Seth reads the news with an oat latte";
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "_pref:news", 10, pref, strlen(pref), NULL, NULL, 0),
                 HU_OK);
    const char own[] = "B shared news about the new puppy";
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "core:b1", 7, own, strlen(own), NULL, k_b, B_LEN),
                 HU_OK);
    return mem;
}

static char *load_for_b(hu_allocator_t *a, hu_memory_t *mem, int mode, size_t *len) {
    hu_confidence_set_mode_for_test(mode);
    hu_retrieval_engine_t eng = hu_retrieval_create(a, mem);
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, a, mem, &eng, 8, 4096), HU_OK);
    char *ctx = NULL;
    *len = 0;
    HU_ASSERT_EQ(hu_memory_loader_load(&loader, "news", 4, k_b, B_LEN, &ctx, len), HU_OK);
    eng.vtable->deinit(eng.ctx, a);
    hu_confidence_set_mode_for_test(-1);
    return ctx;
}

static void experience_write_is_stamped_private_to_its_contact(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = fixture(&a);
    hu_share_level_t level = HU_SHARE_UNSET;
    char c[HU_CB_CONTACT_MAX];
    char key[128] = {0};
    hu_memory_entry_t *rows = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(mem.vtable->list(mem.ctx, &a, NULL, NULL, 0, &rows, &n), HU_OK);
    for (size_t i = 0; i < n; i++) {
        if (rows[i].key && strncmp(rows[i].key, "experience:", 11) == 0)
            snprintf(key, sizeof(key), "%.*s", (int)rows[i].key_len, rows[i].key);
        hu_memory_entry_free_fields(&a, &rows[i]);
    }
    if (rows)
        a.free(a.ctx, rows, n * sizeof(hu_memory_entry_t));
    HU_ASSERT_TRUE(key[0] != '\0'); /* the experience writer stored it */
    HU_ASSERT_TRUE(hu_confidence_repo_lookup(&mem, key, strlen(key), &level, c, sizeof(c)));
    HU_ASSERT_EQ(level, HU_SHARE_PRIVATE_TO_SOURCE);
    HU_ASSERT_STR_EQ(c, k_a);
    HU_ASSERT_TRUE(hu_confidence_repo_lookup(&mem, "_pref:news", 10, &level, c, sizeof(c)));
    HU_ASSERT_EQ(level, HU_SHARE_OWNER_SELF);
    mem.vtable->deinit(mem.ctx);
}

/* Headline: A's confidence is in B's prompt today (OFF), SHADOW is byte for
 * byte the same, LIVE leaves it out and keeps the owner's and B's own rows. */
static void semantic_recall_live_keeps_a_confidence_out_of_b_prompt(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = fixture(&a);
    size_t off_len = 0, sh_len = 0, live_len = 0;
    char *off = load_for_b(&a, &mem, HU_GATE_OFF, &off_len);
    char *sh = load_for_b(&a, &mem, HU_GATE_SHADOW, &sh_len);
    char *live = load_for_b(&a, &mem, HU_GATE_LIVE, &live_len);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_NOT_NULL(strstr(off, "pregnant")); /* the leak this closes */
    HU_ASSERT_NOT_NULL(sh);
    HU_ASSERT_EQ(sh_len, off_len);
    HU_ASSERT_TRUE(memcmp(sh, off, off_len) == 0);
    HU_ASSERT_NOT_NULL(live);
    HU_ASSERT_NULL(strstr(live, "pregnant"));
    HU_ASSERT_NOT_NULL(strstr(live, "oat latte")); /* owner self-fact */
    HU_ASSERT_NOT_NULL(strstr(live, "puppy"));     /* B's own */
    /* the excluded item reached the backstop ledger for B */
    char *draft = dup_str(&a, k_draft);
    size_t dlen = strlen(k_draft);
    hu_confidence_set_mode_for_test(HU_GATE_LIVE);
    HU_ASSERT_EQ(hu_confidence_backstop_apply(&a, k_b, B_LEN, &draft, &dlen), 1);
    hu_confidence_set_mode_for_test(-1);
    HU_ASSERT_NULL(strstr(draft, "pregnant"));
    a.free(a.ctx, draft, dlen + 1);
    a.free(a.ctx, off, off_len + 1);
    a.free(a.ctx, sh, sh_len + 1);
    a.free(a.ctx, live, live_len + 1);
    mem.vtable->deinit(mem.ctx);
}

static void semantic_recall_live_keeps_a_own_confidence_for_a(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = fixture(&a);
    hu_confidence_set_mode_for_test(HU_GATE_LIVE);
    hu_retrieval_engine_t eng = hu_retrieval_create(&a, &mem);
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, &mem, &eng, 8, 4096), HU_OK);
    char *ctx = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_memory_loader_load(&loader, "news", 4, k_a, A_LEN, &ctx, &len), HU_OK);
    hu_confidence_set_mode_for_test(-1);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_NOT_NULL(strstr(ctx, "pregnant"));
    a.free(a.ctx, ctx, len + 1);
    eng.vtable->deinit(eng.ctx, &a);
    mem.vtable->deinit(mem.ctx);
    hu_confidence_ledger_clear();
}

static char *episodes_for_b(hu_allocator_t *a, hu_memory_t *mem, int mode, size_t *len) {
    hu_confidence_set_mode_for_test(mode);
    char *out = NULL;
    *len = 0;
    HU_ASSERT_EQ(hu_episodic_load_for_contact(mem, a, k_b, B_LEN, &out, len), HU_OK);
    hu_confidence_set_mode_for_test(-1);
    return out;
}

static void episodic_live_keeps_a_session_summary_out_of_b_prompt(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    const char sa[] = "ep A said the divorce is final, keep it quiet";
    const char sb[] = "ep B talked about the new puppy";
    HU_ASSERT_EQ(hu_episodic_store(&mem, &a, k_a, A_LEN, sa, strlen(sa)), HU_OK);
    HU_ASSERT_EQ(hu_episodic_store(&mem, &a, k_b, B_LEN, sb, strlen(sb)), HU_OK);
    char *legacy = NULL;
    size_t legacy_len = 0;
    HU_ASSERT_EQ(hu_episodic_load(&mem, &a, &legacy, &legacy_len), HU_OK);
    size_t off_len = 0, sh_len = 0, live_len = 0;
    char *off = episodes_for_b(&a, &mem, HU_GATE_OFF, &off_len);
    char *sh = episodes_for_b(&a, &mem, HU_GATE_SHADOW, &sh_len);
    char *live = episodes_for_b(&a, &mem, HU_GATE_LIVE, &live_len);
    HU_ASSERT_NOT_NULL(legacy);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_EQ(off_len, legacy_len); /* OFF == the unscoped loader */
    HU_ASSERT_TRUE(memcmp(off, legacy, legacy_len) == 0);
    HU_ASSERT_NOT_NULL(strstr(off, "divorce"));
    HU_ASSERT_EQ(sh_len, off_len);
    HU_ASSERT_TRUE(memcmp(sh, off, off_len) == 0);
    HU_ASSERT_NOT_NULL(live);
    HU_ASSERT_NULL(strstr(live, "divorce"));
    HU_ASSERT_NOT_NULL(strstr(live, "puppy"));
    a.free(a.ctx, legacy, legacy_len + 1);
    a.free(a.ctx, off, off_len + 1);
    a.free(a.ctx, sh, sh_len + 1);
    a.free(a.ctx, live, live_len + 1);
    mem.vtable->deinit(mem.ctx);
    hu_confidence_ledger_clear();
}

static size_t commitments_for_b(hu_allocator_t *a, hu_commitment_store_t *cs, int mode,
                                bool *saw_lawyer) {
    hu_confidence_set_mode_for_test(mode);
    hu_commitment_t *list = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_commitment_store_list_active(cs, a, k_b, B_LEN, &list, &n), HU_OK);
    hu_confidence_set_mode_for_test(-1);
    *saw_lawyer = false;
    for (size_t i = 0; i < n; i++) {
        if (list[i].statement && strstr(list[i].statement, "lawyer"))
            *saw_lawyer = true;
        hu_commitment_deinit(&list[i], a);
    }
    if (list)
        a->free(a->ctx, list, n * sizeof(hu_commitment_t));
    return n;
}

static void commitments_live_keep_a_global_promise_out_of_b_prompt(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    hu_commitment_store_t *cs = NULL;
    HU_ASSERT_EQ(hu_commitment_store_create(&a, &mem, &cs), HU_OK);
    hu_commitment_t c;
    memset(&c, 0, sizeof(c));
    c.id = "a1";
    c.statement = "I'll send the divorce papers to your lawyer";
    c.statement_len = strlen(c.statement);
    c.status = HU_COMMITMENT_ACTIVE;
    mem.current_session_id = k_a; /* saved with no session during A's turn */
    mem.current_session_id_len = A_LEN;
    HU_ASSERT_EQ(hu_commitment_store_save(cs, &c, NULL, 0), HU_OK);
    mem.current_session_id = NULL;
    mem.current_session_id_len = 0;
    c.id = "b1";
    c.statement = "I'll bring the puppy toys saturday";
    c.statement_len = strlen(c.statement);
    HU_ASSERT_EQ(hu_commitment_store_save(cs, &c, k_b, B_LEN), HU_OK);
    bool lawyer = false;
    HU_ASSERT_EQ(commitments_for_b(&a, cs, HU_GATE_OFF, &lawyer), 2);
    HU_ASSERT_TRUE(lawyer);
    HU_ASSERT_EQ(commitments_for_b(&a, cs, HU_GATE_SHADOW, &lawyer), 2);
    HU_ASSERT_TRUE(lawyer);
    HU_ASSERT_EQ(commitments_for_b(&a, cs, HU_GATE_LIVE, &lawyer), 1);
    HU_ASSERT_FALSE(lawyer);
    hu_commitment_store_destroy(cs);
    mem.vtable->deinit(mem.ctx);
    hu_confidence_ledger_clear();
}
/* Owner bypass (critic HIGH, PR #608): agent->memory_session_id is the
 * batch's contact for every batch, Seth's own handle included when he texts
 * his twin. Through the real daemon wiring (hu_share_is_owner over the
 * persona), the owner's contact keeps everything in LIVE; a stranger still
 * does not; OFF is byte-identical with or without the wiring. */
static const char k_o[] = "+15550000009"; /* the owner's own handle (fake) */
#define O_LEN (sizeof(k_o) - 1)
static hu_contact_profile_t g_owner_contacts[2];
static hu_persona_t g_owner_persona;
static hu_agent_t g_owner_agent;

static void wire_owner(void) {
    memset(g_owner_contacts, 0, sizeof(g_owner_contacts));
    g_owner_contacts[0].contact_id = (char *)k_b;
    g_owner_contacts[0].relationship = "friend";
    g_owner_contacts[1].contact_id = (char *)k_o;
    g_owner_contacts[1].relationship = "test"; /* the persona's owner marker */
    memset(&g_owner_persona, 0, sizeof(g_owner_persona));
    g_owner_persona.contacts = g_owner_contacts;
    g_owner_persona.contacts_count = 2;
    memset(&g_owner_agent, 0, sizeof(g_owner_agent));
    g_owner_agent.persona = &g_owner_persona;
    hu_daemon_confidence_owner_wire(&g_owner_agent);
}

static char *load_for(hu_allocator_t *a, hu_memory_t *mem, int mode, const char *who,
                      size_t who_len, size_t *len) {
    hu_confidence_set_mode_for_test(mode);
    hu_retrieval_engine_t eng = hu_retrieval_create(a, mem);
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, a, mem, &eng, 8, 4096), HU_OK);
    char *ctx = NULL;
    *len = 0;
    HU_ASSERT_EQ(hu_memory_loader_load(&loader, "news", 4, who, who_len, &ctx, len), HU_OK);
    eng.vtable->deinit(eng.ctx, a);
    hu_confidence_set_mode_for_test(-1);
    return ctx;
}

static void owner_self_chat_live_sees_another_contacts_confidence(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = fixture(&a);
    hu_confidence_ledger_clear();
    size_t off0_len = 0, off_len = 0, live_len = 0, str_len = 0;
    char *off0 = load_for(&a, &mem, HU_GATE_OFF, k_o, O_LEN, &off0_len); /* unwired */
    HU_ASSERT_FALSE(hu_confidence_is_owner_contact(k_o, O_LEN));
    wire_owner();
    HU_ASSERT_TRUE(hu_confidence_is_owner_contact(k_o, O_LEN));
    HU_ASSERT_FALSE(hu_confidence_is_owner_contact(k_b, B_LEN));
    char *off = load_for(&a, &mem, HU_GATE_OFF, k_o, O_LEN, &off_len);
    HU_ASSERT_NOT_NULL(off0);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_EQ(off_len, off0_len); /* OFF: byte-identical with or without the wiring */
    HU_ASSERT_TRUE(memcmp(off, off0, off0_len) == 0);
    char *live = load_for(&a, &mem, HU_GATE_LIVE, k_o, O_LEN, &live_len);
    HU_ASSERT_NOT_NULL(live);
    HU_ASSERT_NOT_NULL(strstr(live, "pregnant")); /* A's private row reaches the owner */
    HU_ASSERT_EQ(live_len, off_len); /* the owner's LIVE prompt == OFF: nothing filtered */
    HU_ASSERT_TRUE(memcmp(live, off, off_len) == 0);
    HU_ASSERT_EQ(hu_confidence_ledger_count(), 0); /* nothing for the backstop to strip */
    char *str = load_for(&a, &mem, HU_GATE_LIVE, k_b, B_LEN, &str_len);
    HU_ASSERT_NOT_NULL(str);
    HU_ASSERT_NULL(strstr(str, "pregnant")); /* a stranger still does not see it */
    hu_daemon_confidence_owner_wire(NULL);
    HU_ASSERT_FALSE(hu_confidence_is_owner_contact(k_o, O_LEN));
    a.free(a.ctx, off0, off0_len + 1);
    a.free(a.ctx, off, off_len + 1);
    a.free(a.ctx, live, live_len + 1);
    a.free(a.ctx, str, str_len + 1);
    mem.vtable->deinit(mem.ctx);
    hu_confidence_ledger_clear();
}
#endif /* HU_ENABLE_SQLITE */

/* Personal model: facts from every contact share one model; "Key facts" and
 * the per-contact walk rendered A's facts while texting B. */
static void add_fact(hu_personal_model_t *pm, const char *subj, const char *pred, const char *obj,
                     const char *handle, const char *channel) {
    hu_heuristic_fact_t *f = &pm->facts[pm->fact_count++];
    memset(f, 0, sizeof(*f));
    snprintf(f->subject, sizeof(f->subject), "%s", subj);
    snprintf(f->predicate, sizeof(f->predicate), "%s", pred);
    snprintf(f->object, sizeof(f->object), "%s", obj);
    f->confidence = 0.9f;
    f->last_seen_at = pm->updated_at;
    if (handle)
        snprintf(f->provenance.contact_handle, sizeof(f->provenance.contact_handle), "%s", handle);
    snprintf(f->provenance.channel, sizeof(f->provenance.channel), "%s", channel);
}

static size_t pm_prompt_for_b(hu_personal_model_t *pm, int mode, char *buf, size_t cap) {
    hu_allocator_t a = hu_system_allocator();
    hu_confidence_set_mode_for_test(mode);
    hu_personal_model_t *owned = NULL;
    const hu_personal_model_t *view = hu_confidence_pm_view(&a, pm, k_b, B_LEN, &owned);
    hu_confidence_set_mode_for_test(-1);
    HU_ASSERT_NOT_NULL(view);
    size_t n = hu_personal_model_build_prompt(view, buf, cap);
    hu_confidence_pm_view_free(&a, owned);
    return n;
}

static void pm_facts_live_keep_a_fact_out_of_b_prompt(void) {
    hu_personal_model_t *pm = (hu_personal_model_t *)calloc(1, sizeof(*pm));
    HU_ASSERT_NOT_NULL(pm);
    hu_personal_model_init(pm);
    pm->updated_at = 1767225600;
    add_fact(pm, "sister", "is", "pregnant", k_a, "imessage_dm");
    add_fact(pm, "user", "likes", "oat lattes", NULL, "cli");
    static char off[8192], sh[8192], live[8192];
    size_t off_n = pm_prompt_for_b(pm, HU_GATE_OFF, off, sizeof(off));
    size_t sh_n = pm_prompt_for_b(pm, HU_GATE_SHADOW, sh, sizeof(sh));
    size_t live_n = pm_prompt_for_b(pm, HU_GATE_LIVE, live, sizeof(live));
    HU_ASSERT_TRUE(off_n > 0 && strstr(off, "pregnant") != NULL);
    HU_ASSERT_EQ(sh_n, off_n);
    HU_ASSERT_TRUE(memcmp(sh, off, off_n) == 0);
    HU_ASSERT_TRUE(live_n > 0);
    HU_ASSERT_NULL(strstr(live, "pregnant"));
    HU_ASSERT_NOT_NULL(strstr(live, "oat lattes")); /* owner self-fact */
    HU_ASSERT_EQ(pm->fact_count, 2);                /* the live model is untouched */
    free(pm);
    hu_confidence_ledger_clear();
}

#ifdef HU_ENABLE_SQLITE
static void pm_facts_live_owner_sees_another_contacts_fact(void) {
    hu_personal_model_t *pm = (hu_personal_model_t *)calloc(1, sizeof(*pm));
    HU_ASSERT_NOT_NULL(pm);
    hu_personal_model_init(pm);
    pm->updated_at = 1767225600;
    add_fact(pm, "sister", "is", "pregnant", k_a, "imessage_dm");
    hu_allocator_t a = hu_system_allocator();
    wire_owner();
    hu_confidence_set_mode_for_test(HU_GATE_LIVE);
    hu_personal_model_t *owned = NULL;
    const hu_personal_model_t *view = hu_confidence_pm_view(&a, pm, k_o, O_LEN, &owned);
    hu_confidence_set_mode_for_test(-1);
    hu_daemon_confidence_owner_wire(NULL);
    HU_ASSERT_TRUE(view == pm); /* unfiltered: no copy */
    HU_ASSERT_NULL(owned);
    static char buf[8192];
    HU_ASSERT_TRUE(hu_personal_model_build_prompt(view, buf, sizeof(buf)) > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "pregnant"));
    HU_ASSERT_EQ(hu_confidence_ledger_count(), 0);
    free(pm);
    hu_confidence_ledger_clear();
}
#endif

void run_confidence_boundary_tests(void) {
    HU_TEST_SUITE("confidence boundary");
    HU_RUN_TEST(derive_row_rules_first_match_wins);
    HU_RUN_TEST(excludes_only_private_items_of_someone_else);
    HU_RUN_TEST(derive_fact_uses_handle_then_owner_channel);
    HU_RUN_TEST(gate_parses_off_shadow_live);
    HU_RUN_TEST(backstop_live_drops_the_sentence_naming_the_confidence);
    HU_RUN_TEST(backstop_shadow_and_off_send_the_draft_unchanged);
    HU_RUN_TEST(backstop_needs_name_and_content_word);
    HU_RUN_TEST(pm_facts_live_keep_a_fact_out_of_b_prompt);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(experience_write_is_stamped_private_to_its_contact);
    HU_RUN_TEST(semantic_recall_live_keeps_a_confidence_out_of_b_prompt);
    HU_RUN_TEST(semantic_recall_live_keeps_a_own_confidence_for_a);
    HU_RUN_TEST(episodic_live_keeps_a_session_summary_out_of_b_prompt);
    HU_RUN_TEST(commitments_live_keep_a_global_promise_out_of_b_prompt);
    HU_RUN_TEST(owner_self_chat_live_sees_another_contacts_confidence);
    HU_RUN_TEST(pm_facts_live_owner_sees_another_contacts_fact);
#endif
}
