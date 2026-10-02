/* Local owner notification. Contract: include/human/daemon/owner_notify.h. */
#include "human/daemon/owner_notify.h"
#include "human/core/allocator.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include <stdio.h>
#include <string.h>

#ifdef HU_IS_TEST
static unsigned g_test_count;
static char g_test_last_body[256];

unsigned hu_owner_notify_test_count(void) {
    return g_test_count;
}
const char *hu_owner_notify_test_last_body(void) {
    return g_test_last_body;
}
void hu_owner_notify_test_reset(void) {
    g_test_count = 0;
    g_test_last_body[0] = '\0';
}
#endif

bool hu_owner_notify_local(const char *body) {
    if (!body || !body[0])
        return false;
#ifdef HU_IS_TEST
    g_test_count++;
    snprintf(g_test_last_body, sizeof(g_test_last_body), "%s", body);
    return true;
#elif defined(__APPLE__) && defined(__MACH__)
    /* The body is a run-handler argument, never part of the script text. */
    static const char k_display[] =
        "display notification (item 1 of argv) with title \"h-uman\" sound name \"default\"";
    const char *argv[] = {
        "/usr/bin/osascript", "-e", "on run argv", "-e", k_display, "-e", "end run", body, NULL};
    hu_allocator_t alloc = hu_system_allocator();
    hu_run_result_t r;
    memset(&r, 0, sizeof(r));
    hu_error_t err = hu_process_run(&alloc, argv, NULL, 4096, &r);
    bool ok = err == HU_OK && r.success;
    hu_run_result_free(&alloc, &r);
    if (!ok)
        hu_log_warn("owner_notify", NULL, "owner notification failed (err=%d)", (int)err);
    return ok;
#else
    hu_log_warn("owner_notify", NULL, "no owner notification on this platform: %s", body);
    return false;
#endif
}
