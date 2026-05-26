#ifndef HU_AGENT_OUTBOUND_SANITIZE_H
#define HU_AGENT_OUTBOUND_SANITIZE_H

/* Outbound-message sanitization for h-uman unsolicited send paths.
 *
 * Background: the 2026-05-26 Annie/Mindy/Betty incident surfaced that
 * the unsolicited send paths (proactive check-in, F25 emotional
 * check-in, temporal follow-up, safety-prepend) push LLM output to
 * the channel WITHOUT a validator. Reactive replies have
 * response_guard.c; unsolicited paths bypassed it.
 *
 * This module is a small pre-channel filter applied at each
 * unsolicited send site. It does two things:
 *
 *   1. STRIP problematic characters from the body (in-place):
 *      - U+FFFC OBJECT REPLACEMENT CHARACTER ("￼"), used by iMessage
 *        as an attachment placeholder. Got included in temporal
 *        follow-ups when chat.db rows with attachments leaked
 *        their placeholder into the LLM's input → output.
 *
 *   2. REJECT messages that LOOK LIKE instruction echoes or
 *      directive-text leaks. The 2026-05-26 evidence:
 *        Annie got "reference something specific you know about them
 *                   or ask about something from a previous conversation"
 *        Mindy got "shared history", "under 10 words", "principle"
 *        Mom got   "[SAFETY] This response touches on violence. ..."
 *      These are PROMPT INSTRUCTIONS that the LLM echoed back as
 *      content. The validator rejects messages that:
 *        - Start with "[SAFETY]" (belt-and-suspenders for the
 *          agent_turn.c:7436 fix landed 2026-05-26 commit 4ba65b6b)
 *        - Match known directive-shaped strings (small list, grows
 *          over time)
 *        - Are a single word matching common instruction-noun list
 *
 * Returns true if message is safe to send; false if it should be
 * dropped. content_len_inout is updated in-place if characters are
 * stripped.
 *
 * Per ~/.claude/rules/silent-config-gated-subsystems.md: rejected
 * sends emit a one-shot info log naming the rejection reason so the
 * operator can grep when something stops sending. */

#include <stdbool.h>
#include <stddef.h>

/* Strip U+FFFC characters and reject instruction-echo / safety-marker
 * leakage. content is mutated in-place; content_len_inout updated.
 *
 * Returns true if safe, false if rejected. On reject, the caller
 * should skip the channel send and log the reason.
 *
 * reason_out (optional, may be NULL): set to a short static string
 * naming why rejected ("safety_marker", "directive_echo",
 * "empty_after_strip"). Borrowed; do not free. */
bool hu_outbound_sanitize(char *content, size_t *content_len_inout, const char **reason_out);

#endif /* HU_AGENT_OUTBOUND_SANITIZE_H */
