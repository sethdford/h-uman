/* Doctor persona_integrity check — pins the 2026-09-06 shape: live persona
 * rewritten to 15 keys / 0 contacts while a backup still holds 24 keys / 15
 * contacts. Doctor must FAIL on that, PASS once restored, PASS with no
 * backups, and NA with no persona name. */

#include "human/core/error.h"
#include "human/doctor/check.h"
#include "human/doctor/check_persona_integrity.h"
#include "human/persona.h"
#include "test_framework.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>

static char s_dir[256];
static char s_old[512];
static bool s_had_old;

static void pi_setup(void) {
    snprintf(s_dir, sizeof(s_dir), "/tmp/hu_test_doctor_persona_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(s_dir));
    const char *old = getenv("HU_PERSONA_DIR");
    s_had_old = old != NULL;
    if (old)
        snprintf(s_old, sizeof(s_old), "%s", old);
    setenv("HU_PERSONA_DIR", s_dir, 1);
    /* Precondition: the check resolves its directory through this override,
     * so a stale env var would silently point it at the real ~/.human. */
    char resolved[512];
    HU_ASSERT_NOT_NULL(hu_persona_base_dir(resolved, sizeof(resolved)));
    HU_ASSERT_STR_EQ(resolved, s_dir);
}

static void pi_teardown(void) {
    if (s_had_old)
        setenv("HU_PERSONA_DIR", s_old, 1);
    else
        unsetenv("HU_PERSONA_DIR");
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

static void put(const char *name, const char *text) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", s_dir, name);
    FILE *f = fopen(p, "wb");
    HU_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

/* 24 keys incl. contacts + proactive, like the real backup. */
static const char *k_full =
    "{\"version\":1,\"name\":\"tp\",\"core\":{},\"core_anchor\":\"x\",\"conflict_style\":{},"
    "\"motivation\":{},\"humor\":{},\"emotional_range\":{},\"voice_rhythm\":{},\"relational\":{},"
    "\"listening\":{},\"repair\":{},\"mirroring\":{},\"social\":{},\"channel_overlays\":[],"
    "\"contacts\":{\"+1\":{\"name\":\"a\"},\"+2\":{\"name\":\"b\"}},\"proactive\":{\"master_"
    "enabled\":true},"
    "\"example_banks\":[],\"life_events\":[],\"style_rules\":[],\"anti_patterns\":[],"
    "\"immersive_reinforcement\":{},\"intellectual\":{},\"sensory\":{}}";

/* The gutted 15-key shape. */
static const char *k_gutted =
    "{\"version\":1,\"name\":\"tp\",\"core\":{},\"core_anchor\":\"x\",\"conflict_style\":{},"
    "\"motivation\":{},\"humor\":{},\"emotional_range\":{},\"voice_rhythm\":{},\"relational\":{},"
    "\"listening\":{},\"repair\":{},\"mirroring\":{},\"social\":{},\"channel_overlays\":[]}";

static hu_doctor_check_result_t run_for(const char *name) {
    hu_doctor_check_persona_integrity_ctx_t ctx = {.cfg = NULL, .persona_name = name};
    return hu_doctor_check_persona_integrity.run(&hu_doctor_check_persona_integrity, &ctx);
}

static void gutted_live_with_full_backup_fails_and_names_lost_keys(void) {
    pi_setup();
    put("tp.json", k_gutted);
    put("tp.json.bak-pre-something", k_full);
    hu_doctor_check_result_t r = run_for("tp");
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_FAIL);
    HU_ASSERT_NOT_NULL(r.reason);
    HU_ASSERT_NOT_NULL(strstr(r.reason, "contacts"));
    HU_ASSERT_NOT_NULL(strstr(r.reason, "proactive"));
    HU_ASSERT_NOT_NULL(strstr(r.reason, "tp.json.bak-pre-something"));
    HU_ASSERT_NOT_NULL(r.detail_json);
    HU_ASSERT_NOT_NULL(strstr(r.detail_json, "\"live_contacts\":0"));
    HU_ASSERT_NOT_NULL(strstr(r.detail_json, "\"backup_contacts\":2"));
    pi_teardown();
}

static void restored_live_passes(void) {
    pi_setup();
    put("tp.json", k_full);
    put("tp.json.bak-pre-something", k_full);
    put("tp.json.bak-older-and-smaller", k_gutted); /* a worse backup must not win */
    hu_doctor_check_result_t r = run_for("tp");
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_PASS);
    HU_ASSERT_NOT_NULL(strstr(r.reason, "2 contacts"));
    pi_teardown();
}

/* A backup from BEFORE a deliberate schema prune carries keys the current
 * schema dropped on purpose. It is older than the newest backup, so it must
 * not be the reference — otherwise every healthy file fails forever. */
static void old_over_full_backup_is_not_the_reference(void) {
    pi_setup();
    put("tp.json", k_gutted);
    put("tp.json.bak-newest", k_gutted); /* newest: same shape as live → healthy */
    put("tp.json.pre-v2-refresh", k_full);
    char p[512];
    snprintf(p, sizeof(p), "%s/tp.json.pre-v2-refresh", s_dir);
    struct utimbuf old_times = {.actime = 1000000000, .modtime = 1000000000}; /* 2001 */
    HU_ASSERT_EQ(utime(p, &old_times), 0);
    hu_doctor_check_result_t r = run_for("tp");
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_PASS);
    HU_ASSERT_NOT_NULL(strstr(r.reason, "tp.json.bak-newest"));
    pi_teardown();
}

static void no_backups_passes_on_live_alone(void) {
    pi_setup();
    put("tp.json", k_gutted); /* nothing to compare against → not a regression */
    hu_doctor_check_result_t r = run_for("tp");
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_PASS);
    pi_teardown();
}

static void writer_temp_file_is_not_a_backup(void) {
    pi_setup();
    put("tp.json", k_gutted);
    put("tp.json.tmp-12345", k_full); /* in-flight temp from creator_write */
    hu_doctor_check_result_t r = run_for("tp");
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_PASS);
    pi_teardown();
}

static void missing_live_file_fails(void) {
    pi_setup();
    hu_doctor_check_result_t r = run_for("tp");
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_FAIL);
    HU_ASSERT_NOT_NULL(strstr(r.reason, "missing"));
    pi_teardown();
}

static void no_persona_name_is_na(void) {
    hu_doctor_check_result_t r = run_for(NULL);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_NA);
}

void run_check_persona_integrity_tests(void) {
    HU_TEST_SUITE("doctor persona_integrity");
    HU_RUN_TEST(gutted_live_with_full_backup_fails_and_names_lost_keys);
    HU_RUN_TEST(restored_live_passes);
    HU_RUN_TEST(old_over_full_backup_is_not_the_reference);
    HU_RUN_TEST(no_backups_passes_on_live_alone);
    HU_RUN_TEST(writer_temp_file_is_not_a_backup);
    HU_RUN_TEST(missing_live_file_fails);
    HU_RUN_TEST(no_persona_name_is_na);
}
