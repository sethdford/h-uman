#include "human/core/privacy.h"
#include <stdatomic.h>

/* Process-global privacy kill-switch; see human/core/privacy.h. Relaxed atomics
 * are sufficient: written once at config load, read concurrently thereafter. */
static atomic_bool g_privacy_enforced = false;

void hu_privacy_set_enforced(bool enforced) {
    atomic_store_explicit(&g_privacy_enforced, enforced, memory_order_relaxed);
}

bool hu_privacy_enforced(void) {
    return atomic_load_explicit(&g_privacy_enforced, memory_order_relaxed);
}
