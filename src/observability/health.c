#include "human/health.h"
#include "human/core/allocator.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#define hu_getpid() (uint32_t)getpid()
#else
#include <pthread.h>
#include <unistd.h>
#define hu_getpid() (uint32_t)getpid()
#endif

#define HU_MAX_COMPONENTS 64
#define HU_MAX_NAME       64

typedef struct health_entry {
    char name[HU_MAX_NAME];
    hu_component_health_t health;
} health_entry_t;

static health_entry_t s_components[HU_MAX_COMPONENTS];
static size_t s_component_count = 0;
static time_t s_start_time = 0;
static bool s_started = false;

#ifndef _WIN32
static pthread_mutex_t s_health_mutex = PTHREAD_MUTEX_INITIALIZER;
#define HEALTH_LOCK()   pthread_mutex_lock(&s_health_mutex)
#define HEALTH_UNLOCK() pthread_mutex_unlock(&s_health_mutex)
#else
#define HEALTH_LOCK()   ((void)0)
#define HEALTH_UNLOCK() ((void)0)
#endif

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
    if (s_component_count >= HU_MAX_COMPONENTS)
        return NULL;
    e = &s_components[s_component_count++];
    strncpy(e->name, component, HU_MAX_NAME - 1);
    e->name[HU_MAX_NAME - 1] = '\0';
    memset(&e->health, 0, sizeof(e->health));
    snprintf(e->health.status, sizeof(e->health.status), "%s", "starting");
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

void hu_health_mark_ok(const char *component) {
    if (!component)
        return;
    HEALTH_LOCK();
    ensure_started();
    health_entry_t *e = get_or_create(component);
    if (!e) {
        HEALTH_UNLOCK();
        return;
    }
    snprintf(e->health.status, sizeof(e->health.status), "%s", "ok");
    timestamp_str(e->health.updated_at, sizeof(e->health.updated_at));
    timestamp_str(e->health.last_ok, sizeof(e->health.last_ok));
    e->health.last_error[0] = '\0';
    HEALTH_UNLOCK();
}

void hu_health_mark_error(const char *component, const char *message) {
    if (!component)
        return;
    HEALTH_LOCK();
    ensure_started();
    health_entry_t *e = get_or_create(component);
    if (!e) {
        HEALTH_UNLOCK();
        return;
    }
    snprintf(e->health.status, sizeof(e->health.status), "%s", "error");
    timestamp_str(e->health.updated_at, sizeof(e->health.updated_at));
    if (message)
        strncpy(e->health.last_error, message, sizeof(e->health.last_error) - 1);
    e->health.last_error[sizeof(e->health.last_error) - 1] = '\0';
    HEALTH_UNLOCK();
}

void hu_health_bump_restart(const char *component) {
    if (!component)
        return;
    HEALTH_LOCK();
    ensure_started();
    health_entry_t *e = get_or_create(component);
    if (!e) {
        HEALTH_UNLOCK();
        return;
    }
    e->health.restart_count++;
    HEALTH_UNLOCK();
}

void hu_health_snapshot(hu_health_snapshot_t *out) {
    if (!out)
        return;
    HEALTH_LOCK();
    ensure_started();
    memset(out, 0, sizeof(*out));
    out->pid = hu_getpid();
    time_t now = time(NULL);
    out->uptime_seconds = (uint64_t)(now > s_start_time ? now - s_start_time : 0);
    if (s_component_count > 0) {
        /* no allocator in scope — raw malloc */
        out->components =
            (hu_component_health_t *)malloc(s_component_count * sizeof(hu_component_health_t));
        if (out->components) {
            out->component_count = s_component_count;
            for (size_t i = 0; i < s_component_count; i++)
                memcpy(&out->components[i], &s_components[i].health, sizeof(hu_component_health_t));
        }
    }
    HEALTH_UNLOCK();
}

hu_readiness_result_t hu_health_check_readiness(hu_allocator_t *alloc) {
    hu_readiness_result_t out = {HU_READINESS_NOT_READY, NULL, 0};
    if (!alloc)
        return out;
    HEALTH_LOCK();
    ensure_started();

    if (s_component_count == 0) {
        out.status = HU_READINESS_READY;
        HEALTH_UNLOCK();
        return out;
    }

    hu_component_check_t *checks = (hu_component_check_t *)alloc->alloc(
        alloc->ctx, s_component_count * sizeof(hu_component_check_t));
    if (!checks) {
        HEALTH_UNLOCK();
        return out;
    }

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
    out.status = all_ok ? HU_READINESS_READY : HU_READINESS_NOT_READY;
    HEALTH_UNLOCK();
    return out;
}

void hu_health_reset(void) {
    HEALTH_LOCK();
    s_component_count = 0;
    s_started = false;
    s_start_time = 0;
    HEALTH_UNLOCK();
}
