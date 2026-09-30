#ifndef HU_DAEMON_PROSPECTIVE_H
#define HU_DAEMON_PROSPECTIVE_H
/*
 * Prospective memory v2 in the reactive path (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.3-4.4).
 *
 * HU_PROSPECTIVE=off|shadow|live, default off:
 *   off    — today's directive, byte-identical: fire on match, mark fired=1.
 *   shadow — today's directive unchanged, plus Filter + Decide run read-only
 *            and log counts ("prospective shadow: candidates=…").
 *   live   — the v2 pass replaces it: soft directive; surfaced -> done only
 *            after the delivered reply carries the action.
 */
#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_provider;

/* "me: …" / "them: …" lines of the last HU_PROSPECTIVE_HISTORY_TURNS entries,
 * oldest first. When they do not all fit in buf[cap], the NEWEST lines that
 * fit are kept (a contiguous recent window). Returns the bytes written. */
size_t hu_daemon_prospective_history_render(const hu_channel_history_entry_t *entries, size_t n,
                                            char *buf, size_t cap);

#ifdef HU_ENABLE_SQLITE
#include "human/memory/prospective_v2.h"

typedef struct hu_daemon_prospective_judge_ctx {
    struct hu_provider *provider;
    const char *model; /* NULL = provider default */
    size_t model_len;
} hu_daemon_prospective_judge_ctx_t;

/* hu_prospective_judge_fn over a provider: one short answer — temperature 0,
 * max_tokens 16, thinking off (hu_provider_chat_oneshot). */
hu_error_t hu_daemon_prospective_provider_judge(void *ctx, hu_allocator_t *alloc,
                                                const char *system, size_t system_len,
                                                const char *user, size_t user_len, char **out,
                                                size_t *out_len);

/* One dated service-log line per pass that judged, expired or capped
 * anything, plus one "item" line per judged intention. `tag`: "shadow",
 * "live", "time shadow", "time live". write_err is the LAST field so
 * parsers of the earlier fields keep working. */
void hu_daemon_prospective_log_counts(const char *tag, const hu_prospective_counts_t *c);

/* The gate dispatch with the mode and judge injected (tests). Returns the
 * directive for the prompt (heap, free with *out_len + 1) or NULL.
 *
 * LIVE ownership: a legacy keyword row with no contact is visible to every
 * contact's keyword read. The contact whose LIVE pass surfaced something owns
 * the surfaced set until its own delivery settles it; a LIVE pass for any
 * OTHER contact first settles it as the owner's undelivered attempt
 * (attempts+1), so another contact's pass or delivery never settles it. */
char *hu_daemon_prospective_directive(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode,
                                      const hu_prospective_turn_t *turn,
                                      const hu_prospective_judge_t *judge, size_t *out_len);

/* After-delivery evidence for the reactive path: LIVE settles the surfaced
 * intentions of the contact that owns them (see above) against the delivered
 * reply, and is a no-op for any other contact; SHADOW logs the uptake of the
 * last would-fires for this contact (no write); OFF does nothing. */
void hu_daemon_prospective_on_delivered(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode,
                                        const char *contact, size_t contact_len, const char *reply,
                                        size_t reply_len, int64_t now);

/* Production entry points: read HU_PROSPECTIVE, judge with the agent's own
 * provider and model, treat the owner's handles (hu_share_is_owner) as
 * self-chat. */
char *hu_daemon_prospective_reactive(hu_allocator_t *alloc, struct hu_agent *agent, sqlite3 *db,
                                     const char *contact, size_t contact_len, const char *text,
                                     size_t text_len, const hu_channel_history_entry_t *history,
                                     size_t history_n, bool is_group, size_t *out_len);
void hu_daemon_prospective_delivered(struct hu_agent *agent, const char *target, size_t target_len,
                                     const char *text, size_t text_len);
#endif /* HU_ENABLE_SQLITE */

#endif /* HU_DAEMON_PROSPECTIVE_H */
