/* src/daemon/daemon_briefing.c — contract in include/human/daemon/briefing.h */
#include "human/agent.h"
#include "human/agent/weather_fetch.h"
#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/core/io_secure.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/paths.h"
#include "human/daemon.h"
#include "human/daemon/briefing.h"
#include "human/daemon_outbound_bus.h"
#include "human/memory.h"
#include "human/memory/superhuman.h"
#include "human/persona.h"
#include "human/persona/temporal.h"
#include "human/platform/calendar.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/briefing_repo.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define OVERDUE_WINDOW_S ((int64_t)7 * 86400) /* older promises stop being mentioned */

/* ── Pure parts ─────────────────────────────────────────────────────────── */

static void clock_str(int hour, int minute, char *buf, size_t cap) {
    int h12 = hour % 12 == 0 ? 12 : hour % 12;
    const char *ap = hour < 12 ? "am" : "pm";
    if (minute == 0)
        snprintf(buf, cap, "%d%s", h12, ap);
    else
        snprintf(buf, cap, "%d:%02d%s", h12, minute, ap);
}

/* The clock time in an AppleScript date string: "… at 10:00:00 AM" or
 * "… 14:30:00". The first H:MM is the time (a date has no colon; the
 * second colon, if any, starts the seconds). */
static void read_clock(const char *s, int *hour, int *minute) {
    *hour = -1;
    *minute = 0;
    const char *best = NULL;
    for (const char *p = s; *p; p++) {
        if (*p != ':' || p == s || !isdigit((unsigned char)p[-1]) ||
            !isdigit((unsigned char)p[1]) || !isdigit((unsigned char)p[2]))
            continue;
        best = p;
        break;
    }
    if (!best)
        return;
    const char *hp = best - 1;
    if (hp > s && isdigit((unsigned char)hp[-1]))
        hp--;
    int h = atoi(hp), m = (best[1] - '0') * 10 + (best[2] - '0');
    const char *q = best + 3;
    if (*q == ':')
        q += 3; /* seconds */
    while (*q == ' ' || *q == '\xe2' || *q == '\x80' || *q == '\xaf')
        q++; /* plain or narrow no-break space before AM/PM */
    if (strncasecmp(q, "pm", 2) == 0 || strncasecmp(q, "p.m", 3) == 0)
        h = h % 12 + 12;
    else if (strncasecmp(q, "am", 2) == 0 || strncasecmp(q, "a.m", 3) == 0)
        h = h % 12;
    if (h < 0 || h > 23 || m > 59)
        return;
    *hour = h;
    *minute = m;
}

static int event_key(const hu_briefing_event_t *e) {
    if (e->all_day)
        return -1;                                           /* all-day first */
    return e->hour < 0 ? 24 * 60 : e->hour * 60 + e->minute; /* unknown times last */
}

static bool json_number(const hu_json_value_t *obj, const char *key, int lo, int hi, int *out) {
    const hu_json_value_t *v = hu_json_object_get(obj, key);
    if (!v || v->type != HU_JSON_NUMBER || v->data.number < lo || v->data.number > hi)
        return false;
    *out = (int)v->data.number;
    return true;
}

bool hu_briefing_parse_calendar(const char *json, size_t len, hu_briefing_event_t *out, size_t cap,
                                size_t *out_n, size_t *out_more) {
    if (out_n)
        *out_n = 0;
    if (out_more)
        *out_more = 0;
    if (!json || !out || cap == 0 || !out_n)
        return false;
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    if (hu_json_parse(&alloc, json, len, &root) != HU_OK || !root || root->type != HU_JSON_ARRAY) {
        if (root)
            hu_json_free(&alloc, root);
        return false;
    }
    size_t more = 0;
    for (size_t i = 0; i < root->data.array.len; i++) {
        const hu_json_value_t *ev = root->data.array.items[i];
        const char *name = ev && ev->type == HU_JSON_OBJECT ? hu_json_get_string(ev, "name") : NULL;
        if (!name || !name[0])
            continue;
        if (*out_n == cap) {
            more++;
            continue;
        }
        hu_briefing_event_t *e = &out[*out_n];
        snprintf(e->name, sizeof(e->name), "%s", name);
        const hu_json_value_t *ad = hu_json_object_get(ev, "allday");
        e->all_day = ad && ad->type == HU_JSON_BOOL && ad->data.boolean;
        int h = -1, m = 0;
        if (e->all_day) {
            e->hour = -1, e->minute = 0;
        } else if (json_number(ev, "h", 0, 23, &h) && json_number(ev, "m", 0, 59, &m)) {
            e->hour = h, e->minute = m;
        } else {
            const char *start = hu_json_get_string(ev, "start");
            read_clock(start ? start : "", &e->hour, &e->minute);
        }
        /* Insertion sort by start time: the helper lists by calendar, not time. */
        for (size_t j = *out_n; j > 0 && event_key(&out[j - 1]) > event_key(&out[j]); j--) {
            hu_briefing_event_t t = out[j - 1];
            out[j - 1] = out[j];
            out[j] = t;
        }
        (*out_n)++;
    }
    hu_json_free(&alloc, root);
    if (out_more)
        *out_more = more;
    return true;
}

bool hu_briefing_in_window(int hour, int send_hour) {
    return send_hour >= 5 && send_hour <= 11 && hour >= send_hour && hour < 12;
}

/* Appends to out; returns false (and leaves *off unchanged) when it would not fit. */
static bool append(char *out, size_t cap, size_t *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static bool append(char *out, size_t cap, size_t *off, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - *off) {
        out[*off] = '\0';
        return false;
    }
    *off += (size_t)w;
    return true;
}

static const char *day_word(int days_away, int64_t now, char *buf, size_t cap) {
    if (days_away == 0)
        return "today";
    if (days_away == 1)
        return "tomorrow";
    static const char *const wd[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                     "Thursday", "Friday", "Saturday"};
    time_t t = (time_t)(now + (int64_t)days_away * 86400);
    struct tm tm;
    if (!localtime_r(&t, &tm))
        return "soon";
    snprintf(buf, cap, "%s", wd[tm.tm_wday]);
    return buf;
}

size_t hu_briefing_compose(const hu_briefing_inputs_t *in, int64_t now, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!in ||
        (in->events_n == 0 && in->reminders_n == 0 && in->commitments_n == 0 && in->dates_n == 0))
        return 0; /* weather alone is not worth a text */
    size_t off = 0;
    bool ok = append(out, cap, &off, "morning. here's today:");
    if (ok && in->events_n > 0) {
        ok = append(out, cap, &off, "\n- on the calendar: ");
        for (size_t i = 0; ok && i < in->events_n; i++) {
            const hu_briefing_event_t *e = &in->events[i];
            char clk[16] = "";
            if (e->hour >= 0)
                clock_str(e->hour, e->minute, clk, sizeof(clk));
            ok = append(out, cap, &off, "%s%s%s%s", i ? ", " : "", clk, clk[0] ? " " : "", e->name);
        }
        if (ok && in->events_more > 0)
            ok = append(out, cap, &off, " (+%zu more)", in->events_more);
    }
    if (ok && in->reminders_n > 0) {
        ok = append(out, cap, &off, "\n- reminders: ");
        for (size_t i = 0; ok && i < in->reminders_n; i++) {
            time_t t = (time_t)in->reminders[i].due_at;
            struct tm tm;
            char clk[16] = "?";
            if (localtime_r(&t, &tm))
                clock_str(tm.tm_hour, tm.tm_min, clk, sizeof(clk));
            ok = append(out, cap, &off, "%s%s (%s)", i ? ", " : "", in->reminders[i].what, clk);
        }
    }
    for (size_t i = 0; ok && i < in->commitments_n; i++) {
        const hu_briefing_commitment_t *c = &in->commitments[i];
        if (c->mine)
            ok = append(out, cap, &off, "\n- you told %s: \"%s\"%s", c->who_name, c->what,
                        c->overdue ? " (overdue)" : "");
        else
            ok = append(out, cap, &off, "\n- %s told you: \"%s\"%s", c->who_name, c->what,
                        c->overdue ? " (still waiting)" : "");
    }
    for (size_t i = 0; ok && i < in->dates_n; i++) {
        char wbuf[16];
        ok = append(out, cap, &off, "\n- %s: %s",
                    day_word(in->dates[i].days_away, now, wbuf, sizeof(wbuf)), in->dates[i].label);
    }
    if (ok && in->have_weather)
        ok = append(out, cap, &off, "\n- %d°F, %s", in->temp_f, in->condition);
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return off;
}

/* ── Daemon side ────────────────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE

static const hu_contact_profile_t *owner_contact(const hu_persona_t *p) {
    for (size_t i = 0; p && i < p->contacts_count; i++) {
        const hu_contact_profile_t *c = &p->contacts[i];
        if (c->contact_id && c->contact_id[0] && c->relationship &&
            strcmp(c->relationship, "test") == 0)
            return c;
    }
    return NULL;
}

/* First name of the contact, or "someone" when it is not a known contact. */
static void contact_first_name(const hu_persona_t *p, const char *contact_id, char *buf,
                               size_t cap) {
    snprintf(buf, cap, "someone");
    for (size_t i = 0; p && i < p->contacts_count; i++) {
        const hu_contact_profile_t *c = &p->contacts[i];
        if (!c->contact_id || strcmp(c->contact_id, contact_id) != 0 || !c->name || !c->name[0])
            continue;
        size_t n = strcspn(c->name, " ");
        snprintf(buf, cap, "%.*s", (int)n, c->name);
        return;
    }
}

static int64_t local_midnight(int64_t now, int add_days) {
    time_t t = (time_t)now;
    struct tm tm;
    if (!localtime_r(&t, &tm))
        return 0;
    tm.tm_mday += add_days;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static void gather(struct hu_agent *agent, sqlite3 *db, const char *owner, int64_t now,
                   hu_briefing_inputs_t *in) {
    memset(in, 0, sizeof(*in));
    const hu_persona_t *p = agent->persona;
    hu_allocator_t sys = hu_system_allocator();
    hu_allocator_t *alloc = agent->alloc ? agent->alloc : &sys;
    int64_t today = local_midnight(now, 0), tomorrow = local_midnight(now, 1);

    hu_reminder_t rows[HU_BRIEFING_MAX_ITEMS];
    size_t n = 0;
    if (hu_reminder_repo_upcoming(db, owner, rows, HU_BRIEFING_MAX_ITEMS, &n) == HU_OK)
        for (size_t i = 0; i < n; i++)
            if (rows[i].due_at < tomorrow)
                in->reminders[in->reminders_n++] = rows[i];

    hu_superhuman_commitment_t *cs = NULL;
    size_t cn = 0;
    if (agent->memory && hu_superhuman_commitment_list_due(agent->memory, alloc, tomorrow - 1, 32,
                                                           &cs, &cn) == HU_OK) {
        for (size_t i = 0; i < cn && in->commitments_n < HU_BRIEFING_MAX_ITEMS; i++) {
            bool mine = strcmp(cs[i].who, "me") == 0;
            if ((!mine && strcmp(cs[i].who, "them") != 0) ||
                cs[i].deadline < today - OVERDUE_WINDOW_S || strcmp(cs[i].contact_id, owner) == 0)
                continue;
            hu_briefing_commitment_t *c = &in->commitments[in->commitments_n++];
            snprintf(c->what, sizeof(c->what), "%s", cs[i].description);
            contact_first_name(p, cs[i].contact_id, c->who_name, sizeof(c->who_name));
            c->mine = mine;
            c->overdue = cs[i].deadline < today;
        }
        hu_superhuman_commitment_free(alloc, cs, cn);
    }

    if (p && p->context_awareness.calendar_enabled) {
        int hours = (int)((tomorrow - now + 3599) / 3600);
        char *json = NULL;
        size_t jl = 0;
        if (hu_calendar_macos_get_events(alloc, hours > 0 ? hours : 1, &json, &jl) == HU_OK &&
            json) {
            if (!hu_briefing_parse_calendar(json, jl, in->events, HU_BRIEFING_MAX_ITEMS,
                                            &in->events_n, &in->events_more))
                hu_log_warn("briefing", agent->observer,
                            "calendar helper returned unparseable JSON; calendar left out");
            alloc->free(alloc->ctx, json, jl + 1);
        }
    }

    if (p && p->important_dates_count > 0) {
        hu_date_entry_t entries[16];
        size_t en = 0;
        for (size_t i = 0; i < p->important_dates_count && en < 16; i++) {
            const hu_important_date_t *d = &p->important_dates[i];
            if (!d->date[0] || d->date[2] != '-')
                continue;
            entries[en].label = d->type;
            entries[en].label_len = strlen(d->type);
            entries[en].month = (d->date[0] - '0') * 10 + (d->date[1] - '0');
            entries[en].day = (d->date[3] - '0') * 10 + (d->date[4] - '0');
            en++;
        }
        time_t t = (time_t)now;
        struct tm tm;
        hu_anniversary_t ann[HU_BRIEFING_MAX_ITEMS];
        size_t an =
            en > 0 && localtime_r(&t, &tm)
                ? hu_temporal_check_anniversaries(entries, en, tm.tm_year + 1900, tm.tm_mon + 1,
                                                  tm.tm_mday, 7, ann, HU_BRIEFING_MAX_ITEMS)
                : 0;
        for (size_t i = 0; i < an; i++) {
            /* Find the source row again for its message ("happy birthday min!"),
             * which is what says whose date it is. */
            const char *msg = "";
            for (size_t k = 0; k < p->important_dates_count; k++)
                if (p->important_dates[k].type == ann[i].label)
                    msg = p->important_dates[k].message;
            hu_briefing_date_t *d = &in->dates[in->dates_n++];
            snprintf(d->label, sizeof(d->label), msg[0] ? "%.*s (\"%s\")" : "%.*s%s",
                     (int)ann[i].label_len, ann[i].label, msg);
            d->days_away = ann[i].days_away;
        }
    }

    if (p && p->context_awareness.weather_enabled && p->location[0]) {
        hu_weather_context_t w;
        memset(&w, 0, sizeof(w));
        if (hu_weather_fetch(alloc, p->location, strlen(p->location), NULL, &w) == HU_OK &&
            w.condition[0]) {
            in->have_weather = true;
            int tenths = w.temp_celsius * 18 + 320; /* °F × 10 */
            in->temp_f = (tenths + (tenths >= 0 ? 5 : -5)) / 10;
            snprintf(in->condition, sizeof(in->condition), "%s", w.condition);
        }
    }
}

static bool write_shadow(const char *day, const char *text, size_t len) {
    char dir[512], path[600];
    if (hu_paths_state(dir, sizeof(dir), "briefings") < 0)
        return false;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return false;
    int w = snprintf(path, sizeof(path), "%s/%s.txt", dir, day);
    if (w < 0 || (size_t)w >= sizeof(path))
        return false;
    /* The briefing quotes private messages and calendar titles: owner-only. */
    FILE *f = NULL;
    if (hu_io_secure_open(path, HU_IO_PERM_SECRET, "w", &f) != HU_OK)
        return false;
    bool ok = fwrite(text, 1, len, f) == len && fputc('\n', f) != EOF;
    return fclose(f) == 0 && ok;
}

static int send_hour(void) {
    const char *v = getenv("HU_BRIEFING_HOUR");
    int h = v && v[0] ? atoi(v) : 8;
    return h >= 5 && h <= 11 ? h : 8;
}

void hu_briefing_tick(struct hu_agent *agent, struct hu_service_channel *channels,
                      size_t channel_count, int64_t now) {
    static int64_t last_check = 0;
    static char done_day[16];
    if (!agent)
        return;
    hu_gate_mode_t gate = hu_gate_mode_from_env("HU_BRIEFING", HU_GATE_OFF);
    if (gate == HU_GATE_OFF) {
        static atomic_bool noted = false;
        hu_log_info_once(&noted, "briefing", agent->observer,
                         "morning briefing off (HU_BRIEFING unset); set HU_BRIEFING=shadow to "
                         "write briefings to the state dir, or live to send them");
        return;
    }
    if (now >= last_check && now - last_check < 60)
        return;
    last_check = now;

    time_t t = (time_t)now;
    struct tm tm;
    if (!localtime_r(&t, &tm) || !hu_briefing_in_window(tm.tm_hour, send_hour()))
        return;
    char day[16];
    snprintf(day, sizeof(day), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    if (strcmp(day, done_day) == 0)
        return;

    const hu_contact_profile_t *owner = owner_contact(agent->persona);
    sqlite3 *db = agent->memory ? hu_sqlite_memory_get_db(agent->memory) : NULL;
    if (!owner || !db) {
        static atomic_bool warned = false;
        hu_log_warn_once(&warned, "briefing", agent->observer,
                         "briefing on but %s; nothing will be sent",
                         owner ? "memory has no SQLite database"
                               : "no persona contact has relationship \"test\" (the owner)");
        return;
    }

    hu_briefing_inputs_t in;
    gather(agent, db, owner->contact_id, now, &in);
    char text[2048];
    size_t len = hu_briefing_compose(&in, now, text, sizeof(text));
    const char *mode = len == 0 ? "empty" : gate == HU_GATE_LIVE ? "live" : "shadow";
    bool claimed = false;
    if (hu_briefing_repo_claim(db, day, now, mode, &claimed) != HU_OK)
        return;
    snprintf(done_day, sizeof(done_day), "%s", day);
    if (!claimed || len == 0) {
        if (claimed)
            hu_log_info("briefing", agent->observer, "nothing worth a briefing on %s", day);
        return;
    }

    if (gate == HU_GATE_SHADOW) {
        bool ok = write_shadow(day, text, len);
        hu_log_info("briefing", agent->observer, "shadow briefing for %s: %zu bytes, %s", day, len,
                    ok ? "written to briefings/" : "COULD NOT be written");
        return;
    }
    const char *chn = owner->proactive_channel && owner->proactive_channel[0]
                          ? owner->proactive_channel
                          : "imessage";
    hu_service_channel_t *sc = hu_daemon_outbound_find_sender(channels, channel_count, chn);
    hu_error_t err = sc ? sc->channel->vtable->send(sc->channel->ctx, owner->contact_id,
                                                    strlen(owner->contact_id), text, len, NULL, 0)
                        : HU_ERR_NOT_FOUND;
    if (err == HU_OK) {
        hu_log_info("briefing", agent->observer, "briefing for %s sent on %s", day, chn);
        return;
    }
    /* Release the day so a later pass (until noon) can try again. */
    (void)hu_briefing_repo_release(db, day);
    done_day[0] = '\0';
    hu_log_warn("briefing", agent->observer, "briefing for %s not sent on %s: %s", day, chn,
                sc ? hu_error_string(err) : "no such channel");
}

#else /* !HU_ENABLE_SQLITE */

void hu_briefing_tick(struct hu_agent *agent, struct hu_service_channel *channels,
                      size_t channel_count, int64_t now) {
    (void)agent, (void)channels, (void)channel_count, (void)now;
}

#endif /* HU_ENABLE_SQLITE */
