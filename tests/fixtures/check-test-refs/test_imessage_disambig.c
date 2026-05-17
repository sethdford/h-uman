/* Fixture: pins the multi-candidate disambiguation in check-test-references.sh.
 *
 * The basename "imessage" matches both src/channels/imessage.c and
 * src/feeds/imessage.c. Before the 2026-05-17 fix, the script used
 * `find ... | head -1` and was filesystem-order-dependent — it would
 * frequently pick src/feeds/imessage.c and demand symbols from there
 * (hu_imessage_extract_attributed_body, hu_imessage_feed_fetch), even
 * for tests that legitimately covered src/channels/imessage.c.
 *
 * This fixture references ONLY a symbol from src/channels/imessage.c
 * (hu_imessage_create) and NONE of the src/feeds/imessage.c exports.
 * The script must pick channels/ via symbol-presence scoring and pass.
 *
 * If the script's disambiguation regresses, this fixture starts failing
 * with "references no production symbol from src/feeds/imessage.c". */

#include "human/channel.h"

static void unused_marker(void) {
    /* The reference below is what scores src/channels/imessage.c over
     * src/feeds/imessage.c. Do not change the symbol name without updating
     * the rationale comment above. */
    extern int hu_imessage_create(void *, const char *, unsigned long, void *, unsigned long,
                                  void *);
    (void)hu_imessage_create;
}
