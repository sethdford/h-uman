#include "human/channels/imessage_caps.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include "human/core/string.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Section labels in `imsg status` output (steipete/imsg 0.11.x). Matched as
 * line prefixes, so a wording change downstream fails CLOSED (no capability)
 * rather than silently mis-reporting one. */
#define CAPS_LABEL_BASIC     "Basic features"
#define CAPS_LABEL_ADVANCED  "Advanced features"
#define CAPS_LABEL_SIP       "System Integrity Protection"
#define CAPS_LABEL_SELECTORS "selectors:"

/* imsg marks selector availability with U+2713 CHECK MARK / U+2717 BALLOT X. */
#define CAPS_MARK_OK "\xE2\x9C\x93"

/* True when the report contains a "<name>: ✓" selector line. Absent or
 * ✗-marked ⇒ false (fail closed — never call a selector that isn't there). */
/* Advance *p to the next line and hand back its whitespace-trimmed extent.
 * Returns false at end of buffer. Every scanner below shares this one loop —
 * three copies of the memchr/trim/advance idiom is exactly what the clone
 * ratchet is there to prevent. */
static bool caps_next_line(const char **p, const char *end, const char **out_s, size_t *out_len) {
    if (!p || !*p || *p >= end)
        return false;
    const char *nl = memchr(*p, '\n', (size_t)(end - *p));
    size_t line_len = nl ? (size_t)(nl - *p) : (size_t)(end - *p);
    const char *s = *p;
    while (line_len > 0 && (*s == ' ' || *s == '\t')) {
        s++;
        line_len--;
    }
    while (line_len > 0 && (s[line_len - 1] == ' ' || s[line_len - 1] == '\r'))
        line_len--;
    *out_s = s;
    *out_len = line_len;
    *p = nl ? nl + 1 : end;
    return true;
}

static bool caps_selector_ok(const char *buf, const char *end, const char *name) {
    size_t name_len = strlen(name);
    const char *p = buf;
    const char *s = NULL;
    size_t n = 0;
    while (caps_next_line(&p, end, &s, &n)) {
        /* Require "<name>:" exactly, so editMessage does not match
         * editMessageItem (the substring trap, again). */
        if (n > name_len && strncasecmp(s, name, name_len) == 0 && s[name_len] == ':')
            return memmem(s, n, CAPS_MARK_OK, sizeof(CAPS_MARK_OK) - 1) != NULL;
    }
    return false;
}

/* Scan forward from `from` for the first non-blank line and report whether it
 * states availability. CRITICAL: "Not available" contains "available" as a
 * substring — the negative MUST be tested first, else every gated box reads as
 * capable and we ship UI puppetry to real contacts. See
 * ~/.claude/rules/substring-classifier-pitfalls.md. */
/* First non-blank line at/after `from`, whitespace-trimmed. Returns false when
 * the region holds no content line. Shared by every value scanner below so the
 * trimming logic exists exactly once. */
static bool caps_first_value_line(const char *from, const char *end, const char **out_s,
                                  size_t *out_len) {
    const char *p = from;
    while (caps_next_line(&p, end, out_s, out_len)) {
        if (*out_len > 0)
            return true;
    }
    return false;
}

/* Read the section's value line and answer a positive/negative word pair.
 * `negative` is tested FIRST and wins — that is what keeps "Not available"
 * from reading as available and "disabled" from reading as enabled. */
static bool caps_line_says(const char *from, const char *end, const char *negative,
                           const char *positive) {
    const char *s = NULL;
    size_t n = 0;
    if (!caps_first_value_line(from, end, &s, &n))
        return false;
    if (negative && hu_str_contains_word_ci_n(s, n, negative))
        return false;
    return hu_str_contains_word_ci_n(s, n, positive);
}

#define caps_value_is_available(from, end) caps_line_says((from), (end), "not", "available")
#define caps_value_is_enabled(from, end)   caps_line_says((from), (end), "disabled", "enabled")

/* Find a line starting with `label` (after leading blanks); returns a pointer
 * just past that line, or NULL. */
static const char *caps_find_section(const char *buf, const char *end, const char *label) {
    size_t label_len = strlen(label);
    const char *p = buf;
    const char *s = NULL;
    size_t n = 0;
    while (caps_next_line(&p, end, &s, &n)) {
        if (n >= label_len && strncasecmp(s, label, label_len) == 0)
            return p; /* just past the label line */
    }
    return NULL;
}

hu_error_t hu_imessage_caps_parse(const char *status_out, size_t len, hu_imessage_caps_t *caps) {
    if (!caps)
        return HU_ERR_INVALID_ARGUMENT;
    memset(caps, 0, sizeof(*caps));
    if (!status_out)
        return HU_ERR_INVALID_ARGUMENT;
    if (len == 0)
        return HU_OK; /* fail closed, not an error */

    const char *end = status_out + len;
    const char *basic = caps_find_section(status_out, end, CAPS_LABEL_BASIC);
    const char *adv = caps_find_section(status_out, end, CAPS_LABEL_ADVANCED);
    const char *sip = caps_find_section(status_out, end, CAPS_LABEL_SIP);

    /* Unrecognized output (wrong tool, error text, version drift) → no
     * capability at all. Never guess. */
    if (!basic && !adv)
        return HU_OK;

    caps->probed = true;
    caps->basic = basic ? caps_value_is_available(basic, end) : false;
    caps->advanced = adv ? caps_value_is_available(adv, end) : false;
    caps->sip_enabled = sip ? caps_value_is_enabled(sip, end) : false;

    /* Per-selector availability, e.g. "    editMessage: ✗". Absent section →
     * selectors_reported stays false and the selector-gated verbs fall back
     * to `advanced` (we cannot know better). */
    if (caps_find_section(status_out, end, CAPS_LABEL_SELECTORS)) {
        caps->selectors_reported = true;
        caps->sel_edit = caps_selector_ok(status_out, end, "editMessage") ||
                         caps_selector_ok(status_out, end, "editMessageItem");
        caps->sel_retract = caps_selector_ok(status_out, end, "retractMessagePart");
    }
    return HU_OK;
}

bool hu_imessage_caps_allows(const hu_imessage_caps_t *caps, hu_imessage_verb_t verb) {
    if (!caps || !caps->probed)
        return false;
    switch (verb) {
    case HU_IMSG_VERB_SEND:
        return caps->basic;
    case HU_IMSG_VERB_REACT:
    case HU_IMSG_VERB_REPLY_THREADED:
    case HU_IMSG_VERB_TYPING:
    case HU_IMSG_VERB_READ_RECEIPT:
    case HU_IMSG_VERB_EFFECT:
        return caps->advanced;
    /* Selector-gated: a live bridge does not guarantee the selector exists.
     * On macOS 26 editMessage is absent while retractMessagePart is present. */
    case HU_IMSG_VERB_EDIT:
        return caps->advanced && (caps->selectors_reported ? caps->sel_edit : true);
    case HU_IMSG_VERB_UNSEND:
        return caps->advanced && (caps->selectors_reported ? caps->sel_retract : true);
    default:
        return false; /* unknown verb fails closed */
    }
}

void hu_imessage_caps_describe(const hu_imessage_caps_t *caps, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return;
    if (!caps) {
        snprintf(buf, cap, "imessage caps: unprobed");
        return;
    }
    snprintf(buf, cap, "imessage caps: basic=%s bridge=%s sip=%s%s", caps->basic ? "yes" : "no",
             caps->advanced ? "yes" : "no", caps->sip_enabled ? "on" : "off",
             caps->advanced ? "" : " (advanced verbs degrade)");
}

/* ── T0.1 blue guard ───────────────────────────────────────────────────── */

hu_imessage_service_t hu_imessage_service_from_string(const char *s, size_t len) {
    if (!s || len == 0)
        return HU_IMSG_SERVICE_UNKNOWN;
    /* Exact token compare — NOT substring. A service column is a closed set;
     * substring matching here would let "SMS-relay" read as SMS etc. */
    if (len == 8 && strncasecmp(s, "iMessage", 8) == 0)
        return HU_IMSG_SERVICE_IMESSAGE;
    if (len == 3 && strncasecmp(s, "SMS", 3) == 0)
        return HU_IMSG_SERVICE_SMS;
    if (len == 3 && strncasecmp(s, "RCS", 3) == 0)
        return HU_IMSG_SERVICE_RCS;
    return HU_IMSG_SERVICE_UNKNOWN;
}

bool hu_imessage_service_is_blue(hu_imessage_service_t svc) {
    return svc == HU_IMSG_SERVICE_IMESSAGE;
}

hu_imessage_service_t hu_imessage_recent_service_email_filter(bool handle_is_email,
                                                              hu_imessage_service_t recent) {
    /* An email address cannot route over SMS or RCS — those are carrier
     * services bound to phone numbers. chat.db nevertheless records
     * SMS-service message rows against email handles (Text Message
     * Forwarding attribution artifacts), and on 2026-07-27 three such rows
     * made the blue guard HOLD the account's own iMessage-active Apple-ID
     * handle. Impossible evidence must not bind: discard it so the verdict
     * falls through to the handle row (which prefers the iMessage row and
     * still fails CLOSED when no iMessage evidence exists at all). */
    if (handle_is_email && (recent == HU_IMSG_SERVICE_SMS || recent == HU_IMSG_SERVICE_RCS))
        return HU_IMSG_SERVICE_UNKNOWN;
    return recent;
}

hu_blue_verdict_t hu_imessage_blue_verdict(hu_imessage_service_t recent_msg_service,
                                           hu_imessage_service_t handle_service) {
    /* Freshest evidence wins: whatever Apple actually routed the last message
     * over is what the next one will use. */
    if (recent_msg_service != HU_IMSG_SERVICE_UNKNOWN)
        return hu_imessage_service_is_blue(recent_msg_service) ? HU_BLUE_ALLOW : HU_BLUE_HOLD;
    /* No message history — fall back to the handle row. */
    if (handle_service != HU_IMSG_SERVICE_UNKNOWN)
        return hu_imessage_service_is_blue(handle_service) ? HU_BLUE_ALLOW : HU_BLUE_HOLD;
    return HU_BLUE_HOLD; /* no evidence ⇒ never risk a green bubble */
}

bool hu_imsg_run_ok(hu_allocator_t *alloc, const char *const *argv, int timeout_s) {
    if (!alloc || !argv)
        return false;
    hu_run_result_t rr = {0};
    hu_error_t e = hu_process_run_with_timeout(alloc, argv, NULL, 65536, timeout_s, &rr);
    bool ok = (e == HU_OK && rr.success && rr.exit_code == 0);
    hu_run_result_free(alloc, &rr);
    return ok;
}

const hu_imessage_caps_t *hu_imessage_caps_cached(hu_allocator_t *alloc) {
    static hu_imessage_caps_t caps;
    static bool probed = false;
    if (!probed && alloc) {
        probed = true;
        (void)hu_imessage_caps_probe(alloc, &caps);
        char desc[224];
        hu_imessage_caps_describe(&caps, desc, sizeof(desc));
        hu_log_info("imessage", NULL, "%s", desc);
    }
    return &caps;
}

/* ── T0.1b live reachability via `imsg whois` ──────────────────────────── */

/* Read integer field `key` when it appears as an OBJECT KEY.
 *
 * Matching the quoted token `"key"` (quotes included) and then REQUIRING a ':'
 * is what keeps this honest in two ways: a bare-substring search for `status`
 * would also hit inside `"id_status"`, and a key name appearing inside a string
 * VALUE is followed by '"' rather than ':'. Cf.
 * ~/.claude/rules/substring-classifier-pitfalls.md.
 *
 * Returns false when the key is absent or its value is not a plain integer —
 * which the caller turns into INDETERMINATE, never into "reachable". */
static bool whois_int_field(const char *s, size_t len, const char *key, long *out) {
    char quoted[24];
    int qn = snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    if (qn <= 0 || (size_t)qn >= sizeof(quoted))
        return false;
    size_t qlen = (size_t)qn;

    for (size_t i = 0; i + qlen <= len; i++) {
        if (memcmp(s + i, quoted, qlen) != 0)
            continue;
        size_t j = i + qlen;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (j >= len || s[j] != ':')
            continue; /* a value, not a key — keep looking */
        j++;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
            j++;
        bool neg = (j < len && s[j] == '-');
        if (neg)
            j++;
        if (j >= len || s[j] < '0' || s[j] > '9')
            return false; /* non-numeric (true/false/string) → fail closed */
        long v = 0;
        while (j < len && s[j] >= '0' && s[j] <= '9') {
            v = v * 10 + (s[j] - '0');
            j++;
        }
        *out = neg ? -v : v;
        return true;
    }
    return false;
}

hu_error_t hu_imessage_whois_parse(const char *json, size_t len, hu_whois_reach_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = HU_WHOIS_INDETERMINATE;
    if (!json || len == 0)
        return HU_OK;

    /* `imsg whois` prints plain-text errors and STILL exits 0, so the payload
     * itself is the only success signal: it must be a JSON object. */
    size_t i = 0;
    while (i < len && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
        i++;
    if (i >= len || json[i] != '{')
        return HU_OK;

    long available = 0, id_status = 0;
    if (!whois_int_field(json, len, "available", &available))
        return HU_OK;
    if (!whois_int_field(json, len, "id_status", &id_status))
        return HU_OK;

    /* BOTH must assert reachability. They agreed across every observed probe;
     * requiring both means future drift in either degrades to not-reachable. */
    *out = (available == 1 && id_status == 1) ? HU_WHOIS_REACHABLE : HU_WHOIS_NOT_REACHABLE;
    return HU_OK;
}

hu_blue_verdict_t hu_imessage_blue_verdict_live(hu_whois_reach_t live,
                                                hu_imessage_service_t recent_msg_service,
                                                hu_imessage_service_t handle_service,
                                                bool negative_is_authoritative) {
    /* The POSITIVE is authoritative: across every probe, only genuinely
     * iMessage-reachable handles ever answered 1 — SMS, RCS and nonsense
     * addresses all answered 0. So a 1 can safely unblock a handle that chat.db
     * knows nothing about (a brand-new contact), which is the capability the
     * chat.db-only guard could never have. */
    if (live == HU_WHOIS_REACHABLE)
        return HU_BLUE_ALLOW;

    /* The NEGATIVE is NOT authoritative by default, because it has measured
     * false negatives: on 2026-07-19 two handles with active 1:1 iMessage
     * threads (134 msgs, newest 3h old; and 13 msgs) answered 0 on repeated
     * probes. Treating that as "don't send" would mute live conversations.
     * Left advisory, it degrades to the chat.db verdict — whose freshest-
     * message rule already catches a contact who genuinely left iMessage,
     * since their recent messages would carry SMS/RCS.
     * Flip via HU_IMESSAGE_WHOIS_STRICT=1 once the false-negative rate has
     * actually been measured. */
    if (live == HU_WHOIS_NOT_REACHABLE && negative_is_authoritative)
        return HU_BLUE_HOLD;

    /* No usable live answer — fall back to the chat.db inference, which itself
     * fails closed when it has no evidence. */
    return hu_imessage_blue_verdict(recent_msg_service, handle_service);
}

/* Per-handle memo. The guard runs on the send hot path and a whois round-trip
 * is a live network lookup, so repeat sends inside the TTL must not re-probe. */
#define WHOIS_CACHE_SLOTS 32
#define WHOIS_CACHE_TTL_S 300
#define WHOIS_HANDLE_MAX  96
#define WHOIS_TIMEOUT_S   5

typedef struct whois_slot {
    char handle[WHOIS_HANDLE_MAX];
    size_t handle_len;
    hu_whois_reach_t reach;
    time_t stamp;
    bool used;
} whois_slot_t;

static whois_slot_t g_whois_cache[WHOIS_CACHE_SLOTS];
static pthread_mutex_t g_whois_mu = PTHREAD_MUTEX_INITIALIZER;

/* Cached answer for `handle`, or INDETERMINATE when absent/expired. */
static bool whois_cache_get(const char *handle, size_t len, time_t now, hu_whois_reach_t *out) {
    bool hit = false;
    pthread_mutex_lock(&g_whois_mu);
    for (size_t i = 0; i < WHOIS_CACHE_SLOTS; i++) {
        whois_slot_t *e = &g_whois_cache[i];
        if (!e->used || e->handle_len != len || memcmp(e->handle, handle, len) != 0)
            continue;
        if (now - e->stamp <= WHOIS_CACHE_TTL_S) {
            *out = e->reach;
            hit = true;
        }
        break;
    }
    pthread_mutex_unlock(&g_whois_mu);
    return hit;
}

static void whois_cache_put(const char *handle, size_t len, time_t now, hu_whois_reach_t reach) {
    pthread_mutex_lock(&g_whois_mu);
    size_t victim = 0;
    time_t oldest = 0;
    bool have_victim = false;
    for (size_t i = 0; i < WHOIS_CACHE_SLOTS; i++) {
        whois_slot_t *e = &g_whois_cache[i];
        if (!e->used || (e->handle_len == len && memcmp(e->handle, handle, len) == 0)) {
            victim = i;
            have_victim = true;
            break;
        }
        if (!have_victim || e->stamp < oldest) {
            oldest = e->stamp;
            victim = i;
        }
    }
    whois_slot_t *e = &g_whois_cache[victim];
    memcpy(e->handle, handle, len);
    e->handle_len = len;
    e->reach = reach;
    e->stamp = now;
    e->used = true;
    pthread_mutex_unlock(&g_whois_mu);
}

hu_whois_reach_t hu_imessage_whois_probe_cached(hu_allocator_t *alloc, const char *handle,
                                                size_t handle_len) {
    if (!alloc || !handle || handle_len == 0 || handle_len >= WHOIS_HANDLE_MAX)
        return HU_WHOIS_INDETERMINATE;

    time_t now = time(NULL);
    hu_whois_reach_t cached = HU_WHOIS_INDETERMINATE;
    if (whois_cache_get(handle, handle_len, now, &cached))
        return cached;

    hu_whois_reach_t reach = HU_WHOIS_INDETERMINATE;
#if defined(__APPLE__) && defined(__MACH__) && !HU_IS_TEST
    char addr[WHOIS_HANDLE_MAX];
    memcpy(addr, handle, handle_len);
    addr[handle_len] = '\0';
    const char *type = memchr(handle, '@', handle_len) ? "email" : "phone";
    const char *argv[] = {"imsg", "whois", "--address", addr, "--type", type, "--json", NULL};
    hu_run_result_t rr = {0};
    hu_error_t err = hu_process_run_with_timeout(alloc, argv, NULL, 8192, WHOIS_TIMEOUT_S, &rr);
    if (err == HU_OK && rr.stdout_buf && rr.stdout_len > 0)
        (void)hu_imessage_whois_parse(rr.stdout_buf, rr.stdout_len, &reach);
    hu_run_result_free(alloc, &rr);
#endif

    /* Cache indeterminate results too: a box with no bridge would otherwise pay
     * a failed spawn on every single send. */
    whois_cache_put(handle, handle_len, now, reach);
    return reach;
}

hu_error_t hu_imessage_caps_probe(hu_allocator_t *alloc, hu_imessage_caps_t *caps) {
    if (!alloc || !caps)
        return HU_ERR_INVALID_ARGUMENT;
    memset(caps, 0, sizeof(*caps));
#if defined(__APPLE__) && defined(__MACH__) && !HU_IS_TEST
    const char *argv[] = {"imsg", "status", NULL};
    hu_run_result_t rr = {0};
    hu_error_t err = hu_process_run_with_timeout(alloc, argv, NULL, 65536, 15, &rr);
    if (err == HU_OK && rr.stdout_buf && rr.stdout_len > 0)
        (void)hu_imessage_caps_parse(rr.stdout_buf, rr.stdout_len, caps);
    hu_run_result_free(alloc, &rr);
#else
    (void)alloc; /* non-macOS / test builds: no bridge, fail closed */
#endif
    return HU_OK;
}
