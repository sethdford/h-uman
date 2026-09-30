/* Commitment-detection precision sheet (src/eval/commitment_sample.c):
 * the sheet the owner labels, the score read back from it, and the
 * `human commitments` command end to end against a real memory database. */
#include "human/core/io_secure.h"
#include "human/eval/commitment_sample.h"
#include "human/memory.h"
#include "human/memory/superhuman.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void fill(hu_superhuman_commitment_t *r, int64_t id, const char *who, const char *contact,
                 const char *said, int64_t deadline) {
    memset(r, 0, sizeof(*r));
    r->id = id;
    snprintf(r->who, sizeof(r->who), "%s", who);
    snprintf(r->contact_id, sizeof(r->contact_id), "%s", contact);
    snprintf(r->description, sizeof(r->description), "%s", said);
    r->deadline = deadline;
}

static void commitment_sheet_has_one_clean_row_per_detection(void) {
    hu_superhuman_commitment_t rows[2];
    fill(&rows[0], 7, "them", "+15550000001", "i'll send\tthe deck\ntomorrow", 0);
    fill(&rows[1], 9, "me", "+15550000002", "call you back", 0);
    hu_contact_profile_t contacts[1];
    memset(contacts, 0, sizeof(contacts));
    contacts[0].contact_id = "+15550000001";
    contacts[0].name = "Mindy Ford";
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.contacts = contacts;
    persona.contacts_count = 1;

    char buf[2048];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    HU_ASSERT_NOT_NULL(f);
    HU_ASSERT_EQ(hu_commitment_sample_write(f, rows, 2, &persona), 2);
    fclose(f);
    /* Tabs and newlines inside what was said cannot break the columns. */
    HU_ASSERT_NOT_NULL(strstr(buf, "label\tid\tdirection\tcontact\tdeadline\tsaid\n"
                                   "\t7\ttheirs\tMindy Ford\t-\ti'll send the deck tomorrow\n"
                                   "\t9\tmine\t+15550000002\t-\tcall you back\n"));

    /* An unlabelled sheet scores as unlabelled, not as 0% or 100%. */
    hu_commitment_score_t s;
    HU_ASSERT_EQ(hu_commitment_sample_score(buf, strlen(buf), &s), HU_OK);
    HU_ASSERT_EQ(s.unlabeled, 2);
    HU_ASSERT_EQ(s.labeled_mine + s.labeled_theirs, 0);
    HU_ASSERT_EQ(s.bad_rows, 0);
}

static void commitment_score_counts_labels_per_direction(void) {
    const char *sheet = "# comments are ignored\n"
                        "label\tid\tdirection\tcontact\tdeadline\tsaid\n"
                        "y\t1\ttheirs\tMindy\t-\ti'll send it\n"
                        "N\t2\ttheirs\tMindy\t-\tsend me a pic\n"
                        " yes\t3\ttheirs\tBetty\t2026-10-01\tpick you up at 5\r\n"
                        "1\t4\tmine\tDana\t-\ti'll call you\n"
                        "0\t5\tmine\tDana\t-\twe should call\n"
                        "\t6\tmine\tDana\t-\tunlabelled\n"
                        "maybe\t7\ttheirs\tMindy\t-\tbad label\n"
                        "y\t8\tsideways\tMindy\t-\tbad direction\n"
                        "\n";
    hu_commitment_score_t s;
    HU_ASSERT_EQ(hu_commitment_sample_score(sheet, strlen(sheet), &s), HU_OK);
    HU_ASSERT_EQ(s.labeled_theirs, 3);
    HU_ASSERT_EQ(s.correct_theirs, 2);
    HU_ASSERT_EQ(s.labeled_mine, 2);
    HU_ASSERT_EQ(s.correct_mine, 1);
    HU_ASSERT_EQ(s.labeled_dated, 1); /* only row 3 has a deadline */
    HU_ASSERT_EQ(s.correct_dated, 1);
    HU_ASSERT_EQ(s.unlabeled, 1);
    HU_ASSERT_EQ(s.bad_rows, 2);
    HU_ASSERT_EQ(hu_commitment_sample_score(NULL, 0, &s), HU_ERR_INVALID_ARGUMENT);
}

static void commitment_command_rejects_what_it_does_not_know(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *bad[] = {"human", "commitments", "tally", NULL};
    HU_ASSERT_EQ(cmd_commitments(&alloc, 3, bad), HU_ERR_INVALID_ARGUMENT);
    char *none[] = {"human", "commitments", NULL};
    HU_ASSERT_EQ(cmd_commitments(&alloc, 2, none), HU_ERR_INVALID_ARGUMENT);
    char *missing[] = {"human", "commitments", "score", "/nonexistent/sheet.tsv", NULL};
    HU_ASSERT_EQ(cmd_commitments(&alloc, 4, missing), HU_ERR_NOT_FOUND);
}

#ifdef HU_ENABLE_SQLITE
static void commitment_command_samples_a_real_database_and_scores_it(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char dir[] = "/tmp/hu_commit_sample_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char db[128], sheet[128];
    snprintf(db, sizeof(db), "%s/memory.db", dir);
    snprintf(sheet, sizeof(sheet), "%s/sheet.tsv", dir);

    hu_memory_t mem = hu_sqlite_memory_create(&alloc, db);
    HU_ASSERT_NOT_NULL(mem.ctx);
    static const char a[] = "i'll send the deck", b[] = "call you tonight";
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000001", 12, a, sizeof(a) - 1,
                                                "them", 4, 0),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000002", 12, b, sizeof(b) - 1,
                                                "me", 2, 0),
                 HU_OK);
    hu_superhuman_commitment_t *rows = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_superhuman_commitment_list_recent(&mem, &alloc, 0, 10, &rows, &n), HU_OK);
    HU_ASSERT_EQ(n, 2);
    HU_ASSERT_STR_EQ(rows[0].description, b); /* newest first */
    hu_superhuman_commitment_free(&alloc, rows, n);
    /* A window that starts after both were recorded holds nothing. */
    HU_ASSERT_EQ(hu_superhuman_commitment_list_recent(&mem, &alloc, (int64_t)time(NULL) + 3600, 10,
                                                      &rows, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, 0);
    hu_superhuman_commitment_free(&alloc, rows, n);
    mem.vtable->deinit(mem.ctx);

    /* An output path that climbs out of its directory is refused, not written. */
    char sneaky[160];
    snprintf(sneaky, sizeof(sneaky), "%s/../hu_commit_sneaky.tsv", dir);
    char *bad_argv[] = {"human", "commitments", "sample", "--db", db, "--out", sneaky, NULL};
    HU_ASSERT_EQ(cmd_commitments(&alloc, 7, bad_argv), HU_ERR_IO);
    HU_ASSERT_EQ(access("/tmp/hu_commit_sneaky.tsv", F_OK), -1);

    char *argv[] = {"human", "commitments", "sample", "--db", db, "--out", sheet, NULL};
    HU_ASSERT_EQ(cmd_commitments(&alloc, 7, argv), HU_OK);
    struct stat st;
    HU_ASSERT_EQ(stat(sheet, &st), 0);
    HU_ASSERT_EQ(st.st_mode & 0777, 0600); /* it quotes private messages */
    FILE *f = fopen(sheet, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[2048];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    HU_ASSERT_NOT_NULL(strstr(buf, "\ttheirs\t+15550000001\t-\ti'll send the deck\n"));
    HU_ASSERT_NOT_NULL(strstr(buf, "\tmine\t+15550000002\t-\tcall you tonight\n"));

    /* The owner labels both rows; score reads the file back. */
    char labelled[2100];
    size_t o = 0;
    for (size_t i = 0; buf[i] && o + 2 < sizeof(labelled); i++) {
        if (buf[i] == '\t' && i > 0 && buf[i - 1] == '\n')
            labelled[o++] = 'y'; /* a row's empty label column gets "y" */
        labelled[o++] = buf[i];
    }
    labelled[o] = '\0';
    f = NULL;
    HU_ASSERT_EQ(hu_io_secure_open(sheet, HU_IO_PERM_SECRET, "w", &f), HU_OK);
    fputs(labelled, f);
    fclose(f);
    hu_commitment_score_t s;
    HU_ASSERT_EQ(hu_commitment_sample_score(labelled, o, &s), HU_OK);
    HU_ASSERT_EQ(s.labeled_theirs, 1);
    HU_ASSERT_EQ(s.correct_theirs, 1);
    HU_ASSERT_EQ(s.labeled_mine, 1);
    HU_ASSERT_EQ(s.bad_rows, 0);
    char *score_argv[] = {"human", "commitments", "score", sheet, NULL};
    HU_ASSERT_EQ(cmd_commitments(&alloc, 4, score_argv), HU_OK);

    unlink(sheet);
    unlink(db);
    char side[160];
    snprintf(side, sizeof(side), "%s-wal", db);
    unlink(side);
    snprintf(side, sizeof(side), "%s-shm", db);
    unlink(side);
    rmdir(dir);
}
#endif

void run_commitment_sample_tests(void) {
    HU_TEST_SUITE("commitment sample");
    HU_RUN_TEST(commitment_sheet_has_one_clean_row_per_detection);
    HU_RUN_TEST(commitment_score_counts_labels_per_direction);
    HU_RUN_TEST(commitment_command_rejects_what_it_does_not_know);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(commitment_command_samples_a_real_database_and_scores_it);
#endif
}
