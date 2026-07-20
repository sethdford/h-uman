/* src/daemon/daemon_promise_keeper.c
 *
 * Outbound half of the promise-keeper ledger. Lives beside the other
 * daemon carve-outs rather than in daemon.c (file-size ceiling ratchet);
 * daemon.c calls hu_daemon_promise_keeper_scan_outbound at the single
 * point where the final reply exists before the choreo/fragment split,
 * so every bubble path is covered by one scan. */

#include "human/daemon/promise_keeper.h"

#include "human/context/conversation.h"
#include "human/core/log.h"
#include "human/memory/superhuman.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <time.h>

hu_promise_keeper_mode_t hu_promise_keeper_mode_from_env(const char *env_value) {
    if (!env_value)
        return HU_PROMISE_KEEPER_OFF;
    if (strcmp(env_value, "on") == 0)
        return HU_PROMISE_KEEPER_LIVE;
    if (strcmp(env_value, "shadow") == 0)
        return HU_PROMISE_KEEPER_SHADOW;
    return HU_PROMISE_KEEPER_OFF;
}

/* Case-insensitive phrase match at desc[pos] with a right word boundary
 * (end-of-string or non-alphanumeric next char), so "let me know" cannot fire
 * inside "let me knowledge..." (substring-classifier-pitfalls). The caller
 * establishes the left boundary. */
static bool phrase_at_word_ci(const char *s, size_t len, size_t pos, const char *needle) {
    size_t nlen = strlen(needle);
    if (pos + nlen > len)
        return false;
    if (strncasecmp(s + pos, needle, nlen) != 0)
        return false;
    return pos + nlen == len || !isalnum((unsigned char)s[pos + nlen]);
}

bool hu_promise_keeper_is_courtesy_invitation(const char *desc, size_t desc_len,
                                              int64_t deadline_ts) {
    if (!desc || desc_len == 0)
        return false;
    /* A parsed deadline means the reply committed to a WHEN — not bare. */
    if (deadline_ts > 0)
        return false;
    size_t start = 0;
    while (start < desc_len && isspace((unsigned char)desc[start]))
        start++;
    /* detect_commitment anchors the description at the earliest keyword, so a
     * courtesy invitation always LEADS with the phrase; matching only at the
     * start keeps "I'll send it over, let me know if it works" out of scope. */
    if (!phrase_at_word_ci(desc, desc_len, start, "let me know"))
        return false;
    /* A first-person deliverable anywhere after the prefix rescues it
     * ("let me know your address and i'll ship the package"). */
    static const char *const deliverable_markers[] = {"i'll", "i will", "i'm going to", "gonna",
                                                      NULL};
    for (size_t i = start; i < desc_len; i++) {
        if (i > 0 && isalnum((unsigned char)desc[i - 1]))
            continue; /* not a left word boundary */
        for (const char *const *kw = deliverable_markers; *kw; kw++)
            if (phrase_at_word_ci(desc, desc_len, i, *kw))
                return false;
    }
    return true;
}

hu_error_t hu_daemon_promise_keeper_scan_outbound(void *memory, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t contact_id_len,
                                                  const char *reply, size_t reply_len,
                                                  hu_promise_keeper_mode_t mode, void *observer,
                                                  bool *stored_out) {
    if (stored_out)
        *stored_out = false;
    if (mode == HU_PROMISE_KEEPER_OFF)
        return HU_OK;
    if (!memory || !alloc || !contact_id || contact_id_len == 0 || !reply || reply_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char desc[512];
    char who[64];
    if (!hu_conversation_detect_commitment(reply, reply_len, desc, sizeof(desc), who, sizeof(who),
                                           /*from_me=*/true))
        return HU_OK;
    /* Only OUR promises belong on the outbound side of the ledger. */
    if (strcmp(who, "me") != 0)
        return HU_OK;

    int64_t deadline = hu_conversation_parse_deadline(reply, reply_len, (int64_t)time(NULL));

    /* Courtesy filter sits BEFORE the mode branch so the SHADOW stream is the
     * filtered stream — that stream's >=80% genuine-commitment precision over
     * a week is the promotion measurement (see header gate comment). */
    if (hu_promise_keeper_is_courtesy_invitation(desc, strlen(desc), deadline))
        return HU_OK;

    if (mode == HU_PROMISE_KEEPER_SHADOW) {
        hu_log_info("human", observer,
                    "[promise-keeper SHADOW] would store '%s' for %.*s (deadline %lld)", desc,
                    (int)(contact_id_len > 20 ? 20 : contact_id_len), contact_id,
                    (long long)deadline);
        return HU_OK;
    }

    hu_error_t err = hu_superhuman_commitment_store(memory, alloc, contact_id, contact_id_len, desc,
                                                    strlen(desc), who, strlen(who), deadline);
    if (err != HU_OK) {
        hu_log_warn("human", observer, "[promise-keeper] store failed (%d) for %.*s", (int)err,
                    (int)(contact_id_len > 20 ? 20 : contact_id_len), contact_id);
        return err;
    }
    if (stored_out)
        *stored_out = true;
    if (deadline > 0) {
        hu_error_t schedule_err = hu_superhuman_delayed_followup_schedule(
            memory, alloc, contact_id, contact_id_len, desc, strlen(desc), deadline);
        if (schedule_err != HU_OK)
            hu_log_warn("human", observer,
                        "[promise-keeper] delayed_followup_schedule failed (%d) for %.*s",
                        (int)schedule_err, (int)(contact_id_len > 20 ? 20 : contact_id_len),
                        contact_id);
    }
    hu_log_info("human", observer, "[promise-keeper] stored '%s' for %.*s (deadline %lld)", desc,
                (int)(contact_id_len > 20 ? 20 : contact_id_len), contact_id, (long long)deadline);
    return HU_OK;
}
