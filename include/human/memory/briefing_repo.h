#ifndef HU_MEMORY_BRIEFING_REPO_H
#define HU_MEMORY_BRIEFING_REPO_H

/* One morning briefing per local day (life-admin slice 2). A day is claimed
 * before the briefing is sent, so a restart mid-morning cannot send it twice;
 * a failed send releases the claim so a later pass can retry. */

#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

hu_error_t hu_briefing_repo_ensure_schema(sqlite3 *db);

/* Claim `day` ("YYYY-MM-DD"). *claimed is false when it was already claimed. */
hu_error_t hu_briefing_repo_claim(sqlite3 *db, const char *day, int64_t now, const char *mode,
                                  bool *claimed);

/* Undo a claim whose send failed. */
hu_error_t hu_briefing_repo_release(sqlite3 *db, const char *day);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_BRIEFING_REPO_H */
