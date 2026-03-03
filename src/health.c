#include "seaclaw/health.h"
#include "seaclaw/core/allocator.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#define sc_getpid() (uint32_t)getpid()
#else
#include <unistd.h>
#define sc_getpid() (uint32_t)getpid()
#endif

#define SC_MAX_COMPONENTS 64
#define SC_MAX_NAME       64

typedef struct health_entry {
    char name[SC_MAX_NAME];
    sc_component_health_t health;
} health_entry_t;

static health_entry_t s_components[SC_MAX_COMPONENTS];
static size_t s_component_count = 0;
static time_t s_start_time = 0;
static bool s_started = false;

static void ensure_started(void) {
    if (!s_started) {
        s_start_time = time(NULL);
        s_started = true;
    }
}

static health_entry_t *find_component(const char *name) {
    for (size_t i = 0; i < s_component_count; i++) {
        if (strcmp(s_components[i].name, name) == 0)
            return &s_components[i];
    }
    return NULL;
}

static health_entry_t *get_or_create(const char *component) {
    health_entry_t *e = find_component(component);
    if (e)
        return e;
    if (s_component_count >= SC_MAX_COMPONENTS)
        return NULL;
    e = &s_components[s_component_count++];
    strncpy(e->name, component, SC_MAX_NAME - 1);
    e->name[SC_MAX_NAME - 1] = '\0';
    memset(&e->health, 0, sizeof(e->health));
    strcpy(e->health.status, "starting");
    return e;
}

static void timestamp_str(char *buf, size_t buf_size) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    if (tm)
        strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", tm);
    else
        snprintf(buf, buf_size, "%ld", (long)t);
}

void sc_health_mark_ok(const char *component) {
    if (!component)
        return;
    ensure_started();
    health_entry_t *e = get_or_create(component);
    if (!e)
        return;
    strcpy(e->health.status, "ok");
    timestamp_str(e->health.updated_at, sizeof(e->health.updated_at));
    timestamp_str(e->health.last_ok, sizeof(e->health.last_ok));
    e->health.last_error[0] = '\0';
}

void sc_health_mark_error(const char *component, const char *message) {
    if (!component)
        return;
    ensure_started();
    health_entry_t *e = get_or_create(component);
    if (!e)
        return;
    strcpy(e->health.status, "error");
    timestamp_str(e->health.updated_at, sizeof(e->health.updated_at));
    if (message)
        strncpy(e->health.last_error, message, sizeof(e->health.last_error) - 1);
    e->health.last_error[sizeof(e->health.last_error) - 1] = '\0';
}

void sc_health_bump_restart(const char *component) {
    if (!component)
        return;
    ensure_started();
    health_entry_t *e = get_or_create(component);
    if (!e)
        return;
    e->health.restart_count++;
}

void sc_health_snapshot(sc_health_snapshot_t *out) {
    if (!out)
        return;
    ensure_started();
    memset(out, 0, sizeof(*out));
    out->pid = sc_getpid();
    time_t now = time(NULL);
    out->uptime_seconds = (uint64_t)(now > s_start_time ? now - s_start_time : 0);
    out->component_count = s_component_count;
    if (s_component_count > 0) {
        out->components =
            (sc_component_health_t *)malloc(s_component_count * sizeof(sc_component_health_t));
        if (out->components) {
            for (size_t i = 0; i < s_component_count; i++)
                memcpy(&out->components[i], &s_components[i].health, sizeof(sc_component_health_t));
        }
    }
}

sc_readiness_result_t sc_health_check_readiness(sc_allocator_t *alloc) {
    sc_readiness_result_t out = {SC_READINESS_NOT_READY, NULL, 0};
    if (!alloc)
        return out;
    ensure_started();

    if (s_component_count == 0) {
        out.status = SC_READINESS_READY;
        return out;
    }

    sc_component_check_t *checks = (sc_component_check_t *)alloc->alloc(
        alloc->ctx, s_component_count * sizeof(sc_component_check_t));
    if (!checks)
        return out;

    bool all_ok = true;
    for (size_t i = 0; i < s_component_count; i++) {
        checks[i].name = s_components[i].name;
        checks[i].healthy = (strcmp(s_components[i].health.status, "ok") == 0);
        checks[i].message =
            s_components[i].health.last_error[0] ? s_components[i].health.last_error : NULL;
        if (!checks[i].healthy)
            all_ok = false;
    }
    out.checks = checks;
    out.check_count = s_component_count;
    out.status = all_ok ? SC_READINESS_READY : SC_READINESS_NOT_READY;
    return out;
}

void sc_health_reset(void) {
    s_component_count = 0;
    s_started = false;
    s_start_time = 0;
}
