/* include/human/channels/imessage_replay_guard.h
 *
 * Pure replay guards from the 2026-09-01 incident. Platform-independent and
 * compiled unconditionally (src/channels/imessage_replay_guard.c) so the
 * unconditional test links where HU_HAS_IMESSAGE is OFF. Included by
 * imessage.h so existing callers see the same declarations. */
#ifndef HU_CHANNELS_IMESSAGE_REPLAY_GUARD_H
#define HU_CHANNELS_IMESSAGE_REPLAY_GUARD_H

#include <stdbool.h>
#include <stdint.h>

/* Pure: decide where the poll cursor resumes on startup.
 *
 * Returns `persisted` when it is a sane cursor no more than `max_replay` rows
 * behind `db_max`; otherwise returns `db_max` (skip the backlog). When the
 * backlog is skipped because it exceeded the cap, *out_skipped receives the
 * number of rows skipped so the caller can log it loudly; it is 0 in every
 * other case. `out_skipped` may be NULL. */
int64_t hu_imessage_resume_rowid(int64_t persisted, int64_t db_max, int64_t max_replay,
                                 int64_t *out_skipped);

/* Pure: true when an inbound message is too old to answer as if it just
 * arrived. An unknown timestamp is never stale: `msg_unix_ts <= 0`, or
 * `<= HU_IMESSAGE_APPLE_EPOCH_UNIX` (chat.db `m.date == 0` converts to exactly
 * 2001-01-01, a missing date, not a 25-year-old message). `max_age_sec == 0`
 * disables the guard. Boundary is inclusive: exactly max_age is NOT stale. */
#define HU_IMESSAGE_APPLE_EPOCH_UNIX 978307200LL
bool hu_imessage_inbound_is_stale(int64_t msg_unix_ts, int64_t now_unix, int64_t max_age_sec);

/* Pure: parse an env override for a guard limit. Returns `dflt` unless `s`
 * is a complete, non-negative decimal integer — garbage must never
 * silently become 0, because 0 disables the guard. */
int64_t hu_imessage_parse_env_int64(const char *s, int64_t dflt);

/* Pure: whether the already-answered guard applies to a polled row. It never
 * applies to the self-chat/loopback handle: every row there is is_from_me=1,
 * so the next self-typed command would look like "human already replied"
 * and swallow the one before it. Case-insensitive (handles may be emails). */
bool hu_imessage_replied_guard_applies(const char *handle, const char *loopback_handle);

#endif /* HU_CHANNELS_IMESSAGE_REPLAY_GUARD_H */
