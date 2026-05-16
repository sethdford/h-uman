/* Fixture: bad.c — a test file that inlines production logic instead of calling it.
 * Used by check-test-references.sh smoke-test (AC-10.4).
 *
 * This file is named test_daemon.c in the smoke-test invocation.  It does NOT
 * reference any hu_* symbol from src/daemon.c; it reimplements a local version.
 * The check script must exit 1 when given this file. */

#include <string.h>

/* Inlined reimplementation — this is the anti-pattern we are guarding against */
static size_t local_strip_channel_tags(char *buf, size_t len) {
    /* ... pretend implementation ... */
    (void)buf;
    return len;
}

static void test_strip_does_something(void) {
    char msg[] = "<ch>hello</ch>";
    local_strip_channel_tags(msg, sizeof(msg) - 1);
}
