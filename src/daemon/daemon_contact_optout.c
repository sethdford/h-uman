/* src/daemon/daemon_contact_optout.c — contract in include/human/daemon_contact_optout.h */
#include "human/daemon_contact_optout.h"
#include "human/agent.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/memory.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/contact_optout_repo.h"
#endif
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Every phrase names the recipient ("me") or is unambiguous on its own, so a
 * bystander sentence about texting in general cannot trip it. Kept short on
 * purpose: a false suppression silences a contact, a miss is one more text. */
static const char *const k_opt_out_phrases[] = {
    "stop texting me",   "stop messaging me", "stop contacting me", "stop reaching out to me",
    "quit texting me",   "don't text me",     "dont text me",       "do not text me",
    "don't message me",  "dont message me",   "do not message me",  "don't contact me",
    "do not contact me", "leave me alone",    "lose my number",     "unsubscribe",
};

/* A negation right before the phrase flips it: "never stop texting me". */
static const char *const k_negations[] = {"don't", "dont", "do not", "never", "won't", "wont"};

static bool negated_before(const char *hay, size_t at) {
    size_t end = at;
    while (end > 0 && (hay[end - 1] == ' ' || hay[end - 1] == ','))
        end--;
    if (end == 0)
        return false;
    for (size_t k = 0; k < sizeof(k_negations) / sizeof(k_negations[0]); k++) {
        size_t nl = strlen(k_negations[k]);
        if (end >= nl && strncasecmp(hay + end - nl, k_negations[k], nl) == 0 &&
            (end == nl || !isalnum((unsigned char)hay[end - nl - 1])))
            return true;
    }
    return false;
}

bool hu_contact_optout_detect(const char *text, size_t len) {
    if (!text || len == 0)
        return false;
    for (size_t p = 0; p < sizeof(k_opt_out_phrases) / sizeof(k_opt_out_phrases[0]); p++) {
        long at = hu_str_find_word_ci_n(text, len, k_opt_out_phrases[p]);
        if (at >= 0 && !negated_before(text, (size_t)at))
            return true;
    }
    return false;
}

bool hu_contact_optout_enabled(void) {
    const char *v = getenv("HU_CONTACT_OPTOUT");
    return !(v && strcmp(v, "off") == 0);
}

#ifdef HU_ENABLE_SQLITE
bool hu_contact_optout_observe_db(struct sqlite3 *db, const char *contact, size_t contact_len,
                                  const char *text, size_t len, int64_t now) {
    if (!db || !contact || contact_len == 0 || !hu_contact_optout_detect(text, len))
        return false;
    char cbuf[128];
    if (contact_len >= sizeof(cbuf))
        contact_len = sizeof(cbuf) - 1;
    memcpy(cbuf, contact, contact_len);
    cbuf[contact_len] = '\0';
    char excerpt[81];
    size_t el = len > 80 ? 80 : len;
    memcpy(excerpt, text, el);
    excerpt[el] = '\0';
    return hu_contact_optout_repo_record(db, now, cbuf, "inbound_opt_out_phrase", excerpt) == HU_OK;
}

bool hu_contact_optout_is_suppressed_db(struct sqlite3 *db, const char *contact) {
    bool s = false;
    if (!db || !contact || !contact[0])
        return false;
    return hu_contact_optout_repo_is_suppressed(db, contact, &s) == HU_OK && s;
}
#endif

bool hu_daemon_contact_optout_observe(struct hu_agent *agent, const char *contact,
                                      size_t contact_len, const char *text, size_t len) {
    if (!hu_contact_optout_enabled())
        return false;
#ifdef HU_ENABLE_SQLITE
    if (!agent || !agent->memory)
        return false;
    struct sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!hu_contact_optout_observe_db(db, contact, contact_len, text, len, (int64_t)time(NULL)))
        return false;
    hu_log_warn("human", agent->observer,
                "[optout] %.*s asked us to stop — proactive contact suppressed from the next "
                "tick (HU_CONTACT_OPTOUT; clear via contact_suppressions)",
                (int)(contact_len > 24 ? 24 : contact_len), contact);
    return true;
#else
    (void)agent;
    (void)contact;
    (void)contact_len;
    (void)text;
    (void)len;
    return false;
#endif
}

bool hu_daemon_contact_optout_should_skip(struct hu_agent *agent, const char *contact) {
    if (!hu_contact_optout_enabled())
        return false;
#ifdef HU_ENABLE_SQLITE
    if (!agent || !agent->memory || !contact)
        return false;
    struct sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!hu_contact_optout_is_suppressed_db(db, contact))
        return false;
    /* Per-process count so a reading is one grep of the service log. */
    static unsigned skipped = 0;
    skipped++;
    hu_log_info("human", agent->observer,
                "[optout] proactive skipped for %.24s — contact opted out [n=%u this process]",
                contact, skipped);
    return true;
#else
    (void)agent;
    (void)contact;
    return false;
#endif
}
