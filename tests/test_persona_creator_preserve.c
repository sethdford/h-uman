/* hu_persona_creator_write must never lose authored persona keys.
 *
 * Pins the 2026-09-06 incident: a partial hu_persona_t (an analyzer response
 * with no contacts) was written over the live persona and the file went from
 * 24 top-level keys to 15 — contacts, proactive, life_events, style_rules and
 * five more vanished, and every proactive path went dark. The writer now
 * streams to a temp file, carries forward every top-level key the new output
 * lacks, and renames atomically. .claude/rules/persona.md: "Extra fields must
 * be preserved (for extensibility)." */

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/persona.h"
#include "test_framework.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_dir[256];
static char s_old_persona_dir[512];
static bool s_had_old;

static void preserve_setup(void) {
    snprintf(s_dir, sizeof(s_dir), "/tmp/hu_test_creator_preserve_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(s_dir));
    const char *old = getenv("HU_PERSONA_DIR");
    s_had_old = old != NULL;
    if (old)
        snprintf(s_old_persona_dir, sizeof(s_old_persona_dir), "%s", old);
    HU_ASSERT_EQ(setenv("HU_PERSONA_DIR", s_dir, 1), 0);
}

static void preserve_teardown(void) {
    if (s_had_old)
        setenv("HU_PERSONA_DIR", s_old_persona_dir, 1);
    else
        unsetenv("HU_PERSONA_DIR");
    /* best-effort cleanup */
    DIR *d = opendir(s_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", s_dir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(s_dir);
}

static void write_file(const char *name, const char *text) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", s_dir, name);
    FILE *f = fopen(p, "wb");
    HU_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

static char *read_file(hu_allocator_t *a, const char *name, size_t *len) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", s_dir, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)a->alloc(a->ctx, (size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

static size_t count_tmp_files(void) {
    size_t n = 0;
    DIR *d = opendir(s_dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strstr(e->d_name, ".tmp-"))
            n++;
    closedir(d);
    return n;
}

/* The authored file: keys the writer knows (core) AND keys it does not
 * (proactive, life_events, style_rules) AND one it knows but the partial
 * struct will not carry (contacts). */
static const char *k_authored =
    "{\n"
    "  \"version\": 1,\n"
    "  \"name\": \"tp\",\n"
    "  \"core\": {\"identity\": \"old identity\", \"traits\": [\"dry\"]},\n"
    "  \"contacts\": {\"+15550001111\": {\"name\": \"Casey\", \"warmth_level\": \"high\"}},\n"
    "  \"proactive\": {\"master_enabled\": true},\n"
    "  \"life_events\": [{\"what\": \"moved\", \"as_of\": \"2026-07-28\"}],\n"
    "  \"style_rules\": [\"no emoji\", \"lowercase\"]\n"
    "}\n";

/* What the analyzer hands the writer: a partial persona with no contacts. */
static const char *k_partial = "{\"name\": \"tp\", \"core\": {\"identity\": \"new identity\"}}";

static void partial_write_preserves_unknown_and_unemitted_keys(void) {
    preserve_setup();
    hu_allocator_t a = hu_system_allocator();
    write_file("tp.json", k_authored);

    hu_persona_t partial;
    memset(&partial, 0, sizeof(partial));
    HU_ASSERT_EQ(hu_persona_load_json(&a, k_partial, strlen(k_partial), &partial), HU_OK);
    HU_ASSERT_EQ(partial.contacts_count, (size_t)0); /* precondition: really partial */
    HU_ASSERT_EQ(hu_persona_creator_write(&a, &partial), HU_OK);
    hu_persona_deinit(&a, &partial);

    size_t len = 0;
    char *text = read_file(&a, "tp.json", &len);
    HU_ASSERT_NOT_NULL(text);
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&a, text, len, &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);

    /* The writer's own field wins where it emitted one ... */
    hu_json_value_t *core = hu_json_object_get(root, "core");
    HU_ASSERT_NOT_NULL(core);
    HU_ASSERT_STR_EQ(hu_json_get_string(core, "identity"), "new identity");

    /* ... and every key it did not emit comes back from the old file. */
    hu_json_value_t *contacts = hu_json_object_get(root, "contacts");
    HU_ASSERT_NOT_NULL(contacts);
    HU_ASSERT_NOT_NULL(hu_json_object_get(contacts, "+15550001111"));
    hu_json_value_t *proactive = hu_json_object_get(root, "proactive");
    HU_ASSERT_NOT_NULL(proactive);
    HU_ASSERT_TRUE(hu_json_get_bool(proactive, "master_enabled", false));
    hu_json_value_t *events = hu_json_object_get(root, "life_events");
    HU_ASSERT_NOT_NULL(events);
    HU_ASSERT_EQ((int)events->type, (int)HU_JSON_ARRAY);
    HU_ASSERT_EQ(events->data.array.len, (size_t)1);
    hu_json_value_t *rules = hu_json_object_get(root, "style_rules");
    HU_ASSERT_NOT_NULL(rules);
    HU_ASSERT_EQ(rules->data.array.len, (size_t)2);

    /* Atomic: no temp file left behind. */
    HU_ASSERT_EQ(count_tmp_files(), (size_t)0);

    /* And the result must still load through the real loader with the
     * preserved contact visible to it. */
    hu_persona_t reloaded;
    memset(&reloaded, 0, sizeof(reloaded));
    HU_ASSERT_EQ(hu_persona_load_json(&a, text, len, &reloaded), HU_OK);
    HU_ASSERT_EQ(reloaded.contacts_count, (size_t)1);
    hu_persona_deinit(&a, &reloaded);

    hu_json_free(&a, root);
    a.free(a.ctx, text, len + 1);
    preserve_teardown();
}

static void full_write_does_not_duplicate_emitted_keys(void) {
    preserve_setup();
    hu_allocator_t a = hu_system_allocator();
    write_file("tp.json", k_authored);

    /* Load the whole authored file, write it back: emitted keys must appear
     * exactly once (no old copy spliced beside the new one). */
    hu_persona_t full;
    memset(&full, 0, sizeof(full));
    HU_ASSERT_EQ(hu_persona_load_json(&a, k_authored, strlen(k_authored), &full), HU_OK);
    HU_ASSERT_EQ(full.contacts_count, (size_t)1);
    HU_ASSERT_EQ(hu_persona_creator_write(&a, &full), HU_OK);
    hu_persona_deinit(&a, &full);

    size_t len = 0;
    char *text = read_file(&a, "tp.json", &len);
    HU_ASSERT_NOT_NULL(text);
    size_t n_contacts = 0, n_core = 0;
    for (const char *p = text; (p = strstr(p, "\"contacts\"")) != NULL; p++)
        n_contacts++;
    for (const char *p = text; (p = strstr(p, "\"core\"")) != NULL; p++)
        n_core++;
    HU_ASSERT_EQ(n_contacts, (size_t)1);
    HU_ASSERT_EQ(n_core, (size_t)1);
    /* unknown keys still rode along */
    HU_ASSERT_NOT_NULL(strstr(text, "\"style_rules\""));
    HU_ASSERT_NOT_NULL(strstr(text, "\"life_events\""));

    a.free(a.ctx, text, len + 1);
    preserve_teardown();
}

static void first_write_and_corrupt_old_file_still_succeed(void) {
    preserve_setup();
    hu_allocator_t a = hu_system_allocator();

    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&a, k_partial, strlen(k_partial), &p), HU_OK);

    /* No existing file: plain write. */
    HU_ASSERT_EQ(hu_persona_creator_write(&a, &p), HU_OK);
    size_t len = 0;
    char *text = read_file(&a, "tp.json", &len);
    HU_ASSERT_NOT_NULL(text);
    HU_ASSERT_NOT_NULL(strstr(text, "new identity"));
    a.free(a.ctx, text, len + 1);

    /* Corrupt existing file: nothing trustworthy to preserve, write proceeds. */
    write_file("tp.json", "{ this is not json");
    HU_ASSERT_EQ(hu_persona_creator_write(&a, &p), HU_OK);
    text = read_file(&a, "tp.json", &len);
    HU_ASSERT_NOT_NULL(text);
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&a, text, len, &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);
    hu_json_free(&a, root);
    a.free(a.ctx, text, len + 1);
    HU_ASSERT_EQ(count_tmp_files(), (size_t)0);

    hu_persona_deinit(&a, &p);
    preserve_teardown();
}

void run_persona_creator_preserve_tests(void) {
    HU_TEST_SUITE("persona creator preserves unknown keys");
    HU_RUN_TEST(partial_write_preserves_unknown_and_unemitted_keys);
    HU_RUN_TEST(full_write_does_not_duplicate_emitted_keys);
    HU_RUN_TEST(first_write_and_corrupt_old_file_still_succeed);
}
