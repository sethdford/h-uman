/*
 * Calendar/Contacts awareness — proactive context for birthdays, meetings, reminders.
 */
#include "seaclaw/multimodal/calendar.h"
#include "seaclaw/core/string.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool contains_keyword(const char *title, size_t title_len, const char *keyword) {
    size_t kw_len = strlen(keyword);
    if (kw_len > title_len)
        return false;
    for (size_t i = 0; i <= title_len - kw_len; i++) {
        bool match = true;
        for (size_t j = 0; j < kw_len; j++) {
            if (tolower((unsigned char)title[i + j]) != tolower((unsigned char)keyword[j])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

sc_event_type_t sc_calendar_detect_type(const char *title, size_t title_len) {
    if (!title || title_len == 0)
        return SC_EVENT_OTHER;

    if (contains_keyword(title, title_len, "birthday") ||
        contains_keyword(title, title_len, "bday"))
        return SC_EVENT_BIRTHDAY;
    if (contains_keyword(title, title_len, "meeting") ||
        contains_keyword(title, title_len, "call") ||
        contains_keyword(title, title_len, "standup") || contains_keyword(title, title_len, "sync"))
        return SC_EVENT_MEETING;
    if (contains_keyword(title, title_len, "remind") || contains_keyword(title, title_len, "todo"))
        return SC_EVENT_REMINDER;
    if (contains_keyword(title, title_len, "deadline") || contains_keyword(title, title_len, "due"))
        return SC_EVENT_DEADLINE;
    if (contains_keyword(title, title_len, "dinner") ||
        contains_keyword(title, title_len, "lunch") ||
        contains_keyword(title, title_len, "party") ||
        contains_keyword(title, title_len, "hangout"))
        return SC_EVENT_SOCIAL;

    return SC_EVENT_OTHER;
}

sc_error_t sc_calendar_add_event(sc_calendar_context_t *ctx, sc_allocator_t *alloc,
                                 const char *title, size_t title_len, sc_event_type_t type,
                                 int64_t start_ts, int64_t end_ts) {
    if (!ctx || !alloc || !title)
        return SC_ERR_INVALID_ARGUMENT;
    if (ctx->event_count >= SC_CALENDAR_MAX_EVENTS)
        return SC_ERR_INVALID_ARGUMENT;

    sc_calendar_event_t *ev = &ctx->events[ctx->event_count];
    ev->title = sc_strndup(alloc, title, title_len);
    if (!ev->title)
        return SC_ERR_OUT_OF_MEMORY;
    ev->title_len = strlen(ev->title);
    ev->type = type;
    ev->start_ts = start_ts;
    ev->end_ts = end_ts;
    ev->location = NULL;
    ev->location_len = 0;
    ev->contact = NULL;
    ev->contact_len = 0;
    ctx->event_count++;
    return SC_OK;
}

static const char *event_type_label(sc_event_type_t type) {
    switch (type) {
    case SC_EVENT_MEETING:
        return "Meeting";
    case SC_EVENT_BIRTHDAY:
        return "Birthday";
    case SC_EVENT_REMINDER:
        return "Reminder";
    case SC_EVENT_DEADLINE:
        return "Deadline";
    case SC_EVENT_SOCIAL:
        return "Social";
    default:
        return "Event";
    }
}

#define SC_CALENDAR_BUF_INIT 512
#define SC_CALENDAR_BUF_GROW 256

static int append_event(char **buf, size_t *cap, size_t *len, const sc_calendar_event_t *ev,
                        int64_t now_ts, sc_allocator_t *alloc) {
    int64_t day_sec = 86400;
    int64_t ev_day = ev->start_ts / day_sec;
    int64_t now_day = now_ts / day_sec;
    int days_ahead = (int)(ev_day - now_day);

    char time_buf[64];
    struct tm tm_buf;
    time_t t = (time_t)ev->start_ts;
    struct tm *tm = gmtime_r(&t, &tm_buf);
    if (tm)
        (void)strftime(time_buf, sizeof(time_buf), "%H:%M", tm);
    else
        (void)snprintf(time_buf, sizeof(time_buf), "?");

    const char *day_label;
    if (days_ahead == 0)
        day_label = "Today";
    else if (days_ahead == 1)
        day_label = "Tomorrow";
    else if (days_ahead >= 2 && days_ahead <= 6)
        day_label = "This week";
    else
        day_label = "Upcoming";

    size_t need = *len + 128;
    if (ev->title)
        need += ev->title_len;
    if (need > *cap) {
        size_t new_cap = *cap + SC_CALENDAR_BUF_GROW;
        while (new_cap < need)
            new_cap += SC_CALENDAR_BUF_GROW;
        char *n = (char *)alloc->realloc(alloc->ctx, *buf, *cap, new_cap);
        if (!n)
            return -1;
        *buf = n;
        *cap = new_cap;
    }

    int n = snprintf(*buf + *len, *cap - *len, "- %s %s: %.*s (%s)\n", day_label, time_buf,
                     (int)(ev->title_len ? ev->title_len : 0), ev->title ? ev->title : "(no title)",
                     event_type_label(ev->type));
    if (n > 0 && (size_t)n < *cap - *len) {
        *len += (size_t)n;
        return 0;
    }
    return -1;
}

sc_error_t sc_calendar_build_context(const sc_calendar_context_t *ctx, sc_allocator_t *alloc,
                                     int64_t now_ts, char **out, size_t *out_len) {
    if (!ctx || !alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;

    int64_t week_end = now_ts + 7 * 86400;
    int64_t day_sec = 86400;
    int64_t now_day = now_ts / day_sec;

    /* Collect events in next 7 days, grouped: today, this week, birthdays */
    size_t cap = SC_CALENDAR_BUF_INIT;
    size_t len = 0;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    int n = snprintf(buf, cap, "### Calendar Awareness\n");
    if (n <= 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return SC_ERR_INVALID_ARGUMENT;
    }
    len = (size_t)n;

    /* Today */
    n = snprintf(buf + len, cap - len, "Today: ");
    if (n > 0 && (size_t)n < cap - len)
        len += (size_t)n;

    bool today_any = false;
    for (size_t i = 0; i < ctx->event_count; i++) {
        const sc_calendar_event_t *ev = &ctx->events[i];
        if (ev->start_ts < now_ts || ev->start_ts > week_end)
            continue;
        int64_t ev_day = ev->start_ts / day_sec;
        if (ev_day == now_day) {
            if (append_event(&buf, &cap, &len, ev, now_ts, alloc) != 0) {
                alloc->free(alloc->ctx, buf, cap);
                return SC_ERR_OUT_OF_MEMORY;
            }
            today_any = true;
        }
    }
    if (!today_any) {
        n = snprintf(buf + len, cap - len, "(none)\n");
        if (n > 0 && (size_t)n < cap - len)
            len += (size_t)n;
    }

    /* This week (excluding today) */
    n = snprintf(buf + len, cap - len, "This week: ");
    if (n > 0 && (size_t)n < cap - len)
        len += (size_t)n;

    bool week_any = false;
    for (size_t i = 0; i < ctx->event_count; i++) {
        const sc_calendar_event_t *ev = &ctx->events[i];
        if (ev->start_ts < now_ts || ev->start_ts > week_end)
            continue;
        int64_t ev_day = ev->start_ts / day_sec;
        if (ev_day != now_day) {
            if (append_event(&buf, &cap, &len, ev, now_ts, alloc) != 0) {
                alloc->free(alloc->ctx, buf, cap);
                return SC_ERR_OUT_OF_MEMORY;
            }
            week_any = true;
        }
    }
    if (!week_any) {
        n = snprintf(buf + len, cap - len, "(none)\n");
        if (n > 0 && (size_t)n < cap - len)
            len += (size_t)n;
    }

    /* Birthdays */
    n = snprintf(buf + len, cap - len, "Birthdays: ");
    if (n > 0 && (size_t)n < cap - len)
        len += (size_t)n;

    bool bday_any = false;
    for (size_t i = 0; i < ctx->event_count; i++) {
        const sc_calendar_event_t *ev = &ctx->events[i];
        if (ev->type != SC_EVENT_BIRTHDAY || ev->start_ts < now_ts || ev->start_ts > week_end)
            continue;
        if (append_event(&buf, &cap, &len, ev, now_ts, alloc) != 0) {
            alloc->free(alloc->ctx, buf, cap);
            return SC_ERR_OUT_OF_MEMORY;
        }
        bday_any = true;
    }
    if (!bday_any) {
        n = snprintf(buf + len, cap - len, "(none)\n");
        if (n > 0 && (size_t)n < cap - len)
            len += (size_t)n;
    }

    size_t need = len + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, cap, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, cap);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = shrunk;
    *out_len = len;
    return SC_OK;
}

void sc_calendar_deinit(sc_calendar_context_t *ctx, sc_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    for (size_t i = 0; i < ctx->event_count; i++) {
        sc_calendar_event_t *ev = &ctx->events[i];
        if (ev->title) {
            sc_str_free(alloc, ev->title);
            ev->title = NULL;
        }
        ev->title_len = 0;
        if (ev->location) {
            sc_str_free(alloc, ev->location);
            ev->location = NULL;
        }
        ev->location_len = 0;
        if (ev->contact) {
            sc_str_free(alloc, ev->contact);
            ev->contact = NULL;
        }
        ev->contact_len = 0;
    }
    ctx->event_count = 0;
}
