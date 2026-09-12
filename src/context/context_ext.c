#include "human/context/context_ext.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HU_CONTEXT_EXT_ESCAPE_BUF 1024
#define HU_CONTEXT_EXT_SQL_BUF    4096

/* --- F47: Content Forwarding --- */
static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap, size_t *out_len) {
    (void)hu_sql_quote_escape_into(s, len, buf, cap, out_len);
}
hu_error_t hu_forwarding_query_for_contact_sql(const char *contact_id, size_t len, char *buf,
                                               size_t cap, size_t *out_len) {
    if (!contact_id || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_CONTEXT_EXT_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, len, contact_esc, sizeof(contact_esc), &ce_len);

    int n = snprintf(buf, cap,
                     "SELECT id, content, source, topic, received_at, share_score, shared_with "
                     "FROM shareable_content WHERE (shared_with IS NULL OR shared_with NOT LIKE "
                     "'%%' || '%s' || '%%') ORDER BY share_score DESC LIMIT 20",
                     contact_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}
/* --- F52: Current Events --- */
hu_error_t hu_events_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] = "CREATE TABLE IF NOT EXISTS current_events (\n"
                              "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
                              "    topic TEXT NOT NULL,\n"
                              "    summary TEXT NOT NULL,\n"
                              "    source TEXT NOT NULL,\n"
                              "    published_at INTEGER NOT NULL,\n"
                              "    relevance REAL NOT NULL\n"
                              ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}
#ifdef HU_IS_TEST
#elif defined(HU_ENABLE_FEEDS)

#include "human/feeds/news.h"

#else
#endif

/* --- F55-F57: Group Chat --- */