/* src/daemon/daemon_person_dates.c — contract in include/human/daemon/person_dates.h */
#include "human/agent.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon/person_dates.h"
#include "human/daemon/share_queue.h"
#include "human/memory.h"
#include "human/persona.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/person_dates_repo.h"
#endif

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *const k_months[] = {"january",   "february", "march",    "april",
                                       "may",       "june",     "july",     "august",
                                       "september", "october",  "november", "december"};

/* 1..12 for a month name or its 3+ letter prefix ("mar", "sept"), else 0. */
static int month_of(const char *w) {
    size_t n = strlen(w);
    if (n < 3)
        return 0;
    for (int m = 0; m < 12; m++)
        if (strncmp(k_months[m], w, n) == 0)
            return m + 1;
    return 0;
}

/* A day number with an optional ordinal suffix: "3", "3rd", "21st". */
static int day_of(const char *w) {
    int d = 0, digits = 0;
    while (isdigit((unsigned char)w[digits]) && digits < 2)
        d = d * 10 + (w[digits++] - '0');
    if (digits == 0)
        return 0;
    const char *rest = w + digits;
    if (*rest && strcmp(rest, "st") != 0 && strcmp(rest, "nd") != 0 && strcmp(rest, "rd") != 0 &&
        strcmp(rest, "th") != 0)
        return 0;
    return d;
}

static bool is_year(const char *w) {
    return strlen(w) == 4 && isdigit((unsigned char)w[0]) && isdigit((unsigned char)w[3]) &&
           atoi(w) >= 1900 && atoi(w) <= 2100;
}

static bool valid_day(int month, int day) {
    static const int max[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month >= 1 && month <= 12 && day >= 1 && day <= max[month - 1];
}

/* The whole of `s` is one date. Separators: space, '/', '-', ','. */
static bool parse_date(const char *s, int *month, int *day) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", s);
    char *w[6];
    size_t n = 0;
    for (char *tok = strtok(buf, " /-,."); tok && n < 6; tok = strtok(NULL, " /-,."))
        w[n++] = tok;
    if (n > 0 && is_year(w[n - 1]))
        n--; /* a trailing year is fine and ignored */
    int m = 0, d = 0;
    if (n == 2 && isdigit((unsigned char)w[0][0]) && isdigit((unsigned char)w[1][0])) {
        m = day_of(w[0]), d = day_of(w[1]); /* "3/3", "03-03": month first (US) */
    } else if (n == 2 && (m = month_of(w[0])) != 0) {
        d = day_of(w[1]); /* "march 3rd" */
    } else if (n == 2) {
        d = day_of(w[0]), m = month_of(w[1]); /* "3 march" */
    } else if (n == 3 && strcmp(w[1], "of") == 0) {
        d = day_of(w[0]), m = month_of(w[2]); /* "3rd of march" */
    }
    if (!valid_day(m, d))
        return false;
    *month = m, *day = d;
    return true;
}

bool hu_person_date_parse(const char *text, size_t len, hu_person_date_cmd_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!text || len == 0 || len > 256)
        return false;
    /* Lowercase, curly apostrophes made straight, trailing punctuation dropped. */
    char s[300];
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < sizeof(s); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == 0xe2 && i + 2 < len && (unsigned char)text[i + 1] == 0x80 &&
            (unsigned char)text[i + 2] == 0x99) {
            s[n++] = '\'';
            i += 2;
        } else {
            s[n++] = (char)tolower(c);
        }
    }
    while (n > 0 && strchr(" .!\n\r\t", s[n - 1]))
        n--;
    s[n] = '\0';
    char *p = s;
    while (*p == ' ')
        p++;
    static const char *const lead[] = {"remember that ", "remember ", "fyi, ", "fyi ",
                                       "note: ",         "note ",     "btw, ", "btw ",
                                       "heads up, ",     "heads up "};
    for (size_t k = 0; k < sizeof(lead) / sizeof(lead[0]); k++)
        if (strncmp(p, lead[k], strlen(lead[k])) == 0) {
            p += strlen(lead[k]);
            break;
        }

    char *after;
    if (strncmp(p, "my ", 3) == 0 || strncmp(p, "our ", 4) == 0) {
        size_t wl = p[0] == 'm' ? 2 : 3;
        snprintf(out->who, sizeof(out->who), "%.*s", (int)wl, p);
        after = p + wl + 1;
    } else {
        char *poss = strstr(p, "'s ");
        if (!poss || poss == p || (size_t)(poss - p) >= sizeof(out->who))
            return false;
        int spaces = 0;
        for (char *q = p; q < poss; q++)
            spaces += *q == ' ';
        if (spaces > 2)
            return false; /* a person, not a sentence */
        snprintf(out->who, sizeof(out->who), "%.*s", (int)(poss - p), p);
        after = poss + 3;
    }

    static const char *const labels[][2] = {{"birthday", "birthday"},
                                            {"bday", "birthday"},
                                            {"b-day", "birthday"},
                                            {"anniversary", "anniversary"}};
    const char *label = NULL;
    for (size_t k = 0; k < 4; k++) {
        size_t ll = strlen(labels[k][0]);
        if (strncmp(after, labels[k][0], ll) == 0 && (after[ll] == ' ' || after[ll] == ':')) {
            label = labels[k][1];
            after += ll;
            break;
        }
    }
    if (!label)
        return false;
    while (*after == ' ' || *after == ':' || *after == '=')
        after++;
    static const char *const joins[] = {"is on ", "falls on ", "is ", "on "};
    for (size_t k = 0; k < 4; k++)
        if (strncmp(after, joins[k], strlen(joins[k])) == 0) {
            after += strlen(joins[k]);
            break;
        }
    if (!parse_date(after, &out->month, &out->day))
        return false;
    snprintf(out->label, sizeof(out->label), "%s", label);
    return true;
}

static bool is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int64_t local_noon(int year, int month, int day) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = (month == 2 && day == 29 && !is_leap(year)) ? 28 : day;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    return (int64_t)mktime(&t);
}

int hu_person_date_days_away(int month, int day, int64_t now) {
    if (!valid_day(month, day))
        return -1;
    time_t tn = (time_t)now;
    struct tm tm;
    if (!localtime_r(&tn, &tm))
        return -1;
    int year = tm.tm_year + 1900;
    int64_t today = local_noon(year, tm.tm_mon + 1, tm.tm_mday);
    int64_t next = local_noon(year, month, day);
    if (next < today)
        next = local_noon(year + 1, month, day);
    double days = (double)(next - today) / 86400.0;
    return (int)(days + 0.5); /* noon to noon: a DST hour rounds away */
}

/* ── Daemon side ────────────────────────────────────────────────────────── */

static hu_gate_mode_t dates_gate(void) {
    return hu_gate_mode_from_env("HU_DATES", HU_GATE_OFF);
}

#ifdef HU_ENABLE_SQLITE

static const char *const k_month_names[] = {"January",   "February", "March",    "April",
                                            "May",       "June",     "July",     "August",
                                            "September", "October",  "November", "December"};

static const hu_contact_profile_t *contact_by_id(const hu_persona_t *p, const char *id) {
    for (size_t i = 0; p && i < p->contacts_count; i++)
        if (p->contacts[i].contact_id && strcmp(p->contacts[i].contact_id, id) == 0)
            return &p->contacts[i];
    return NULL;
}

/* "Betty's birthday", "your anniversary". */
static void person_label(const hu_persona_t *p, const char *contact_id, const char *label,
                         char *buf, size_t cap) {
    const hu_contact_profile_t *c = contact_by_id(p, contact_id);
    if (c && c->relationship && strcmp(c->relationship, "test") == 0) {
        snprintf(buf, cap, "your %s", label);
    } else if (c && c->name && c->name[0]) {
        size_t n = strcspn(c->name, " ");
        snprintf(buf, cap, "%.*s's %s", (int)n, c->name, label);
    } else {
        snprintf(buf, cap, "%s (%s)", label, contact_id);
    }
}

bool hu_person_dates_handle_owner_message(struct hu_agent *agent, const char *owner,
                                          size_t owner_len, const char *text, size_t len,
                                          int64_t now, char *reply, size_t reply_cap) {
    if (!agent || !owner || !text || !reply || reply_cap == 0)
        return false;
    reply[0] = '\0';
    hu_gate_mode_t gate = dates_gate();
    if (gate == HU_GATE_OFF || !agent->persona ||
        !hu_share_is_owner(agent->persona, owner, owner_len))
        return false;
    hu_person_date_cmd_t cmd;
    if (!hu_person_date_parse(text, len, &cmd))
        return false;
    if (gate == HU_GATE_SHADOW) {
        hu_log_info("dates", agent->observer, "shadow: would store a %s for %zu-char name",
                    cmd.label, strlen(cmd.who));
        return false;
    }
    sqlite3 *db = agent->memory ? hu_sqlite_memory_get_db(agent->memory) : NULL;
    if (!db)
        return false;
    char handle[128];
    if (strcmp(cmd.who, "my") == 0 || strcmp(cmd.who, "our") == 0) {
        snprintf(handle, sizeof(handle), "%.*s", (int)owner_len, owner);
    } else if (!hu_share_resolve_contact(agent->persona, cmd.who, handle, sizeof(handle))) {
        snprintf(reply, reply_cap,
                 "who's %s? I couldn't match that to exactly one person in your contacts", cmd.who);
        return true;
    }
    if (hu_person_dates_repo_set(db, handle, cmd.label, cmd.month, cmd.day, "owner_text", now) !=
        HU_OK) {
        snprintf(reply, reply_cap, "couldn't save that one, try again in a sec?");
        return true;
    }
    char who[96];
    person_label(agent->persona, handle, cmd.label, who, sizeof(who));
    snprintf(reply, reply_cap, "got it: %s, %s %d", who, k_month_names[cmd.month - 1], cmd.day);
    return true;
}

typedef struct candidate {
    char contact_id[128];
    char label[32];
    int days_away;
} candidate_t;

static bool have(const candidate_t *c, size_t n, const char *contact_id, const char *label) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(c[i].contact_id, contact_id) == 0 && strcmp(c[i].label, label) == 0)
            return true;
    return false;
}

static void add_contacts_birthdays(struct hu_agent *agent, int64_t now, int window,
                                   candidate_t *cand, size_t cap, size_t *n) {
    const char *dir = getenv("HU_ADDRESSBOOK_DIR");
    char buf[512];
#ifdef HU_IS_TEST
    if (!dir)
        return; /* tests never read the real Contacts */
#endif
    if (!dir) {
        /* Kept: Contacts is not state, so hu_paths_* does not apply. */
        const char *home = getenv("HOME");
        if (!home)
            return;
        snprintf(buf, sizeof(buf), "%s/Library/Application Support/AddressBook", home);
        dir = buf;
    }
    static hu_addressbook_birthday_t bdays[512];
    size_t bn = 0, unreadable = 0;
    if (hu_addressbook_birthdays(dir, bdays, 512, &bn, &unreadable) != HU_OK)
        return;
    if (unreadable > 0) {
        static atomic_bool warned = false;
        hu_log_warn_once(&warned, "dates", agent->observer,
                         "%zu Contacts database(s) could not be read; grant the daemon Contacts "
                         "access in System Settings > Privacy to see birthdays from Contacts",
                         unreadable);
    }
    const hu_persona_t *p = agent->persona;
    for (size_t i = 0; p && i < p->contacts_count && *n < cap; i++) {
        const hu_contact_profile_t *c = &p->contacts[i];
        char d[16];
        hu_person_dates_phone_key(c->contact_id, d, sizeof(d));
        if (strlen(d) < 7 || have(cand, *n, c->contact_id, "birthday"))
            continue;
        for (size_t k = 0; k < bn; k++) {
            if (strcmp(bdays[k].digits, d) != 0)
                continue;
            int away = hu_person_date_days_away(bdays[k].month, bdays[k].day, now);
            if (away >= 0 && away <= window) {
                candidate_t *x = &cand[(*n)++];
                snprintf(x->contact_id, sizeof(x->contact_id), "%s", c->contact_id);
                snprintf(x->label, sizeof(x->label), "birthday");
                x->days_away = away;
            }
            break;
        }
    }
}

size_t hu_person_dates_upcoming(struct hu_agent *agent, int64_t now, int window_days,
                                hu_briefing_date_t *out, size_t cap) {
    if (!agent || !out || cap == 0 || dates_gate() != HU_GATE_LIVE)
        return 0;
    sqlite3 *db = agent->memory ? hu_sqlite_memory_get_db(agent->memory) : NULL;
    candidate_t cand[64];
    size_t n = 0;
    if (db) {
        hu_person_date_t rows[64];
        size_t rn = 0;
        if (hu_person_dates_repo_list(db, rows, 64, &rn) == HU_OK)
            for (size_t i = 0; i < rn; i++) {
                int away = hu_person_date_days_away(rows[i].month, rows[i].day, now);
                if (away < 0 || away > window_days)
                    continue;
                candidate_t *x = &cand[n++];
                snprintf(x->contact_id, sizeof(x->contact_id), "%s", rows[i].contact_id);
                snprintf(x->label, sizeof(x->label), "%s", rows[i].label);
                x->days_away = away;
            }
    }
    /* Owner-given dates came first, so they win over Contacts for a person. */
    add_contacts_birthdays(agent, now, window_days, cand, 64, &n);
    for (size_t i = 1; i < n; i++) /* soonest first; small n */
        for (size_t j = i; j > 0 && cand[j - 1].days_away > cand[j].days_away; j--) {
            candidate_t t = cand[j - 1];
            cand[j - 1] = cand[j];
            cand[j] = t;
        }
    size_t k = 0;
    for (; k < n && k < cap; k++) {
        person_label(agent->persona, cand[k].contact_id, cand[k].label, out[k].label,
                     sizeof(out[k].label));
        out[k].days_away = cand[k].days_away;
    }
    return k;
}

#else /* !HU_ENABLE_SQLITE */

bool hu_person_dates_handle_owner_message(struct hu_agent *agent, const char *owner,
                                          size_t owner_len, const char *text, size_t len,
                                          int64_t now, char *reply, size_t reply_cap) {
    (void)agent, (void)owner, (void)owner_len, (void)text, (void)len, (void)now;
    if (reply && reply_cap)
        reply[0] = '\0';
    (void)dates_gate;
    return false;
}

size_t hu_person_dates_upcoming(struct hu_agent *agent, int64_t now, int window_days,
                                hu_briefing_date_t *out, size_t cap) {
    (void)agent, (void)now, (void)window_days, (void)out, (void)cap;
    return 0;
}

#endif /* HU_ENABLE_SQLITE */
