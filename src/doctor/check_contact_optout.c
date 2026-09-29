/* src/doctor/check_contact_optout.c — see include/human/doctor/check_ops.h
 *
 * October roadmap O5 gate: "suppression count on the card". A contact who
 * asked us to stop must be visible to the operator, not only in a log line.
 * PASS at any count — the number is the point; FAIL only when the table
 * cannot be read at all (then nobody knows who opted out). */
#include "human/core/allocator.h"
#include "human/doctor/check_ops.h"
#include "human/memory.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/contact_optout_repo.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HU_ENABLE_SQLITE
/* Only the SQLite path formats into these; without it the check returns a
 * literal N/A string, and file-scope statics would trip -Werror=unused-variable
 * on the no-sqlite build (see check_reflection_loop.c for the same guard). */
static char s_reason[320];
static char s_detail[96];
#endif

static hu_doctor_check_result_t run(hu_doctor_check_t *self, void *vctx) {
    (void)self;
    const hu_doctor_contact_optout_ctx_t *ctx = (const hu_doctor_contact_optout_ctx_t *)vctx;
#ifdef HU_ENABLE_SQLITE
    char pb[512];
    const char *path =
        hu_doctor_ops_home_path(ctx ? ctx->memory_db : NULL, pb, sizeof(pb), ".human/memory.db");
    if (!path || access(path, R_OK) != 0) {
        snprintf(
            s_reason, sizeof(s_reason),
            "memory.db not readable at %s — cannot tell who has opted out of proactive contact",
            path ? path : "?");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, NULL};
    }
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, path);
    if (!mem.ctx) {
        snprintf(s_reason, sizeof(s_reason), "could not open %s", path);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, NULL};
    }
    int64_t n = -1;
    hu_error_t e = hu_contact_optout_repo_count(hu_sqlite_memory_get_db(&mem), &n);
    mem.vtable->deinit(mem.ctx);
    if (e != HU_OK) {
        snprintf(s_reason, sizeof(s_reason), "contact_suppressions count failed in %s", path);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, NULL};
    }
    const char *env = getenv("HU_CONTACT_OPTOUT");
    bool off = env && strcmp(env, "off") == 0;
    snprintf(s_detail, sizeof(s_detail), "{\"suppressed_contacts\":%lld,\"enforced\":%s}",
             (long long)n, off ? "false" : "true");
    snprintf(s_reason, sizeof(s_reason), "%lld contact(s) have opted out of proactive contact%s",
             (long long)n,
             off ? " — HU_CONTACT_OPTOUT=off: NOT enforced (escape hatch is set)"
                 : " (honoured within one turn)");
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason, s_detail};
#else
    (void)ctx;
    return (hu_doctor_check_result_t){HU_DOCTOR_NA, "built without SQLite — no suppression table",
                                      NULL};
#endif
}

const hu_doctor_check_t hu_doctor_check_contact_optout = {
    "contact_optout", "Contacts who asked us to stop texting first (O5 contestability)", run, NULL,
    NULL};
