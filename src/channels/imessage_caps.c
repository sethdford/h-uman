#include "human/channels/imessage_caps.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include "human/core/string.h"

#include <stdio.h>
#include <string.h>

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
