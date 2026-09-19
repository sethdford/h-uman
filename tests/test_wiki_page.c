/* tests/test_wiki_page.c
 *
 * Per-contact wiki page (src/memory/wiki_page.c) and the memory loader's
 * HU_WIKI_HEAD block (src/agent/memory_loader.c). Pins: file-name safety of
 * the contact id, the line-boundary slice, the recall-cap offset that keeps
 * a LIVE page from growing the prompt, the read contract (missing page vs
 * empty page vs unsafe id), and the loader gate (off/shadow leave the prompt
 * untouched, live appends the page head). Pages are read through
 * hu_paths_state, so the fixture points HU_STATE_DIR at a scratch dir. */
#include "test_framework.h"

#include "human/agent/memory_loader.h"
#include "human/core/allocator.h"
#include "human/memory/wiki_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory/engines.h"
#endif

static const char k_contact[] = "+15550000001";
static const char k_page[] = "# Brea (broker)\n"
                             "## open threads\n"
                             "- review lease agreement (cue: lease) [pm:12]\n"
                             "## what I remember\n"
                             "- asking rent $4500, pet fee refundable (Sep 2026) [ins:7]\n";

typedef struct wiki_fixture {
    char dir[256];
    char page_path[512];
    char *saved_state;
} wiki_fixture_t;

static void fixture_up(wiki_fixture_t *fx) {
    memset(fx, 0, sizeof(*fx));
    const char *old = getenv("HU_STATE_DIR");
    fx->saved_state = old ? strdup(old) : NULL;
    snprintf(fx->dir, sizeof(fx->dir), "/tmp/hu-wiki-test-XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(fx->dir));
    char wiki[320];
    snprintf(wiki, sizeof(wiki), "%s/wiki", fx->dir);
    HU_ASSERT_EQ(mkdir(wiki, 0700), 0);
    snprintf(fx->page_path, sizeof(fx->page_path), "%s/%s.md", wiki, k_contact);
    FILE *f = fopen(fx->page_path, "wb");
    HU_ASSERT_NOT_NULL(f);
    fwrite(k_page, 1, sizeof(k_page) - 1, f);
    fclose(f);
    setenv("HU_STATE_DIR", fx->dir, 1);
}

static void fixture_down(wiki_fixture_t *fx) {
    unlink(fx->page_path);
    char wiki[320];
    snprintf(wiki, sizeof(wiki), "%s/wiki", fx->dir);
    rmdir(wiki);
    rmdir(fx->dir);
    if (fx->saved_state) {
        setenv("HU_STATE_DIR", fx->saved_state, 1);
        free(fx->saved_state);
    } else {
        unsetenv("HU_STATE_DIR");
    }
}

static void contact_id_safety_refuses_paths_and_hidden_names(void) {
    HU_ASSERT_TRUE(hu_wiki_contact_id_is_safe("+15550000001", 12));
    HU_ASSERT_TRUE(hu_wiki_contact_id_is_safe("edisonsford@icloud.com", 22));
    HU_ASSERT_TRUE(hu_wiki_contact_id_is_safe("self", 4));
    HU_ASSERT_FALSE(hu_wiki_contact_id_is_safe("../etc", 6));
    HU_ASSERT_FALSE(hu_wiki_contact_id_is_safe("a/b", 3));
    HU_ASSERT_FALSE(hu_wiki_contact_id_is_safe("a b", 3));
    HU_ASSERT_FALSE(hu_wiki_contact_id_is_safe(".hidden", 7));
    HU_ASSERT_FALSE(hu_wiki_contact_id_is_safe("a..b", 4));
    HU_ASSERT_FALSE(hu_wiki_contact_id_is_safe("", 0));
    HU_ASSERT_FALSE(hu_wiki_contact_id_is_safe(NULL, 3));
}

static void slice_keeps_whole_lines_only(void) {
    static const char page[] = "a\nbb\nccc\n"; /* 9 bytes */
    HU_ASSERT_EQ(hu_wiki_page_slice(page, 9, 9), (size_t)9);
    HU_ASSERT_EQ(hu_wiki_page_slice(page, 9, 100), (size_t)9);
    HU_ASSERT_EQ(hu_wiki_page_slice(page, 9, 5), (size_t)5); /* "a\nbb\n" */
    HU_ASSERT_EQ(hu_wiki_page_slice(page, 9, 4), (size_t)2); /* "a\n" */
    HU_ASSERT_EQ(hu_wiki_page_slice(page, 9, 1), (size_t)0);
    HU_ASSERT_EQ(hu_wiki_page_slice(page, 9, 0), (size_t)0);
    HU_ASSERT_EQ(hu_wiki_page_slice("no newline", 10, 4), (size_t)0);
    HU_ASSERT_EQ(hu_wiki_page_slice(NULL, 0, 4), (size_t)0);
}

static void recall_cap_drops_by_page_bytes_with_a_floor(void) {
    HU_ASSERT_EQ(hu_wiki_recall_cap(4096, 0), (size_t)4096);
    HU_ASSERT_EQ(hu_wiki_recall_cap(4096, 1000), (size_t)3096);
    HU_ASSERT_EQ(hu_wiki_recall_cap(4096, 2048), (size_t)2048);
    HU_ASSERT_EQ(hu_wiki_recall_cap(4096, 3000), (size_t)2048); /* floored */
    HU_ASSERT_EQ(hu_wiki_recall_cap(4096, 9000), (size_t)2048);
}

static void read_returns_budgeted_head_missing_or_invalid(void) {
    wiki_fixture_t fx;
    fixture_up(&fx);
    hu_allocator_t a = hu_system_allocator();
    char *out = (char *)0x1;
    size_t len = 99;

    /* whole page fits: trailing newline stripped, length matches strlen */
    HU_ASSERT_EQ(hu_wiki_page_read(&a, k_contact, strlen(k_contact), 1200, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(len, sizeof(k_page) - 2);
    HU_ASSERT_EQ(strlen(out), len);
    HU_ASSERT_TRUE(strstr(out, "[ins:7]") != NULL);
    a.free(a.ctx, out, len + 1);

    /* small budget: only whole lines survive */
    HU_ASSERT_EQ(hu_wiki_page_read(&a, k_contact, strlen(k_contact), 40, &out, &len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "# Brea (broker)\n## open threads");
    a.free(a.ctx, out, len + 1);

    /* budget below the first line: HU_OK, nothing */
    HU_ASSERT_EQ(hu_wiki_page_read(&a, k_contact, strlen(k_contact), 3, &out, &len), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(len, (size_t)0);

    /* no page for this contact */
    HU_ASSERT_EQ(hu_wiki_page_read(&a, "+15550000002", 12, 1200, &out, &len), HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(out);

    /* unsafe id never touches the filesystem */
    HU_ASSERT_EQ(hu_wiki_page_read(&a, "../wiki/x", 9, 1200, &out, &len), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(hu_wiki_page_read(&a, k_contact, strlen(k_contact), 1200, NULL, &len),
                 HU_ERR_INVALID_ARGUMENT);
    fixture_down(&fx);
}

#ifdef HU_ENABLE_SQLITE
static void loader_block_follows_the_wiki_gate(void) {
    wiki_fixture_t fx;
    fixture_up(&fx);
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    hu_memory_loader_t loader;
    HU_ASSERT_EQ(hu_memory_loader_init(&loader, &a, &mem, NULL, 8, 4096), HU_OK);

    static const int modes[] = {HU_GATE_OFF, HU_GATE_SHADOW, HU_GATE_LIVE};
    for (size_t i = 0; i < 3; i++) {
        hu_memory_loader_set_wiki_mode_for_test(modes[i]);
        char *ctx = NULL;
        size_t ctx_len = 0;
        HU_ASSERT_EQ(
            hu_memory_loader_load(&loader, "hey", 3, k_contact, strlen(k_contact), &ctx, &ctx_len),
            HU_OK);
        bool has_block = ctx && strstr(ctx, "Your page on them") != NULL;
        bool has_line = ctx && strstr(ctx, "[ins:7]") != NULL;
        if (modes[i] == HU_GATE_LIVE) {
            HU_ASSERT_TRUE(has_block);
            HU_ASSERT_TRUE(has_line);
            HU_ASSERT_EQ(strlen(ctx), ctx_len);
        } else {
            HU_ASSERT_FALSE(has_block); /* off and shadow leave the prompt untouched */
            HU_ASSERT_FALSE(has_line);
        }
        if (ctx)
            a.free(a.ctx, ctx, ctx_len + 1);
    }

    /* live, but a contact without a page: nothing appended, no error */
    hu_memory_loader_set_wiki_mode_for_test(HU_GATE_LIVE);
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_memory_loader_load(&loader, "hey", 3, "+15550000002", 12, &ctx, &ctx_len),
                 HU_OK);
    HU_ASSERT_TRUE(!ctx || strstr(ctx, "Your page on them") == NULL);
    if (ctx)
        a.free(a.ctx, ctx, ctx_len + 1);

    hu_memory_loader_set_wiki_mode_for_test(-1);
    HU_ASSERT_EQ((int)hu_memory_loader_wiki_mode(), (int)HU_GATE_OFF); /* env unset → off */
    mem.vtable->deinit(mem.ctx);
    fixture_down(&fx);
}
#endif

void run_wiki_page_tests(void) {
    HU_TEST_SUITE("wiki page (sleep-time consolidation head)");
    HU_RUN_TEST(contact_id_safety_refuses_paths_and_hidden_names);
    HU_RUN_TEST(slice_keeps_whole_lines_only);
    HU_RUN_TEST(recall_cap_drops_by_page_bytes_with_a_floor);
    HU_RUN_TEST(read_returns_budgeted_head_missing_or_invalid);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(loader_block_follows_the_wiki_gate);
#endif
}
