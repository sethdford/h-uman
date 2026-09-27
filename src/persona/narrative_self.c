#include "human/persona/narrative_self.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void hu_narrative_self_init(hu_narrative_self_t *self) {
    if (!self)
        return;
    memset(self, 0, sizeof(*self));
}

void hu_narrative_self_deinit(hu_allocator_t *alloc, hu_narrative_self_t *self) {
    if (!alloc || !self)
        return;

    hu_str_free(alloc, self->identity_statement);
    self->identity_statement = NULL;

    for (size_t i = 0; i < self->theme_count; i++) {
        hu_str_free(alloc, self->themes[i]);
        self->themes[i] = NULL;
    }
    self->theme_count = 0;

    for (size_t i = 0; i < self->origin_count; i++) {
        hu_str_free(alloc, self->origin_stories[i]);
        self->origin_stories[i] = NULL;
    }
    self->origin_count = 0;

    for (size_t i = 0; i < self->growth_count; i++) {
        hu_str_free(alloc, self->growth_arcs[i]);
        self->growth_arcs[i] = NULL;
    }
    self->growth_count = 0;

    hu_str_free(alloc, self->current_preoccupation);
    self->current_preoccupation = NULL;
    self->last_reflection_ts = 0;
}

hu_error_t hu_narrative_self_set_identity(hu_allocator_t *alloc, hu_narrative_self_t *self,
                                          const char *statement, size_t len) {
    if (!alloc || !self)
        return HU_ERR_INVALID_ARGUMENT;

    const char *src = statement ? statement : "";
    char *copy = hu_strndup(alloc, src, len);
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;

    hu_str_free(alloc, self->identity_statement);
    self->identity_statement = copy;
    return HU_OK;
}

hu_error_t hu_narrative_self_add_theme(hu_allocator_t *alloc, hu_narrative_self_t *self,
                                       const char *theme, size_t len) {
    if (!alloc || !self)
        return HU_ERR_INVALID_ARGUMENT;
    if (self->theme_count >= HU_NARRATIVE_MAX_THEMES)
        return HU_ERR_LIMIT_REACHED;

    const char *src = theme ? theme : "";
    char *copy = hu_strndup(alloc, src, len);
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;

    self->themes[self->theme_count++] = copy;
    return HU_OK;
}

hu_error_t hu_narrative_self_add_growth_arc(hu_allocator_t *alloc, hu_narrative_self_t *self,
                                            const char *arc, size_t len) {
    if (!alloc || !self)
        return HU_ERR_INVALID_ARGUMENT;
    if (self->growth_count >= HU_NARRATIVE_MAX_GROWTH_ARCS)
        return HU_ERR_LIMIT_REACHED;

    const char *src = arc ? arc : "";
    char *copy = hu_strndup(alloc, src, len);
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;

    self->growth_arcs[self->growth_count++] = copy;
    return HU_OK;
}

hu_error_t hu_narrative_self_set_preoccupation(hu_allocator_t *alloc, hu_narrative_self_t *self,
                                               const char *text, size_t len) {
    if (!alloc || !self)
        return HU_ERR_INVALID_ARGUMENT;

    const char *src = text ? text : "";
    char *copy = hu_strndup(alloc, src, len);
    if (!copy)
        return HU_ERR_OUT_OF_MEMORY;

    hu_str_free(alloc, self->current_preoccupation);
    self->current_preoccupation = copy;
    return HU_OK;
}

static size_t narrative_append(char *buf, size_t cap, size_t off, const char *fmt, ...) {
    if (off >= cap)
        return off;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return off;
    if ((size_t)n >= cap - off)
        return cap;
    return off + (size_t)n;
}

hu_error_t hu_narrative_self_build_context(hu_allocator_t *alloc, const hu_narrative_self_t *self,
                                           char **out, size_t *out_len) {
    if (!alloc || !self || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[2048];
    size_t off = 0;
    off = narrative_append(buf, sizeof buf, off, "[NARRATIVE SELF:");

    if (self->identity_statement && self->identity_statement[0]) {
        off = narrative_append(buf, sizeof buf, off, " Identity: %s.", self->identity_statement);
    }

    if (self->theme_count > 0U) {
        off = narrative_append(buf, sizeof buf, off, " Themes:");
        for (size_t i = 0; i < self->theme_count; i++) {
            if (!self->themes[i])
                continue;
            off = narrative_append(buf, sizeof buf, off, i == 0U ? " %s" : ", %s", self->themes[i]);
        }
        off = narrative_append(buf, sizeof buf, off, ".");
    }

    if (self->growth_count > 0U) {
        off = narrative_append(buf, sizeof buf, off, " Growth arcs:");
        for (size_t i = 0; i < self->growth_count; i++) {
            if (!self->growth_arcs[i])
                continue;
            off = narrative_append(buf, sizeof buf, off, i == 0U ? " %s" : ", %s",
                                   self->growth_arcs[i]);
        }
        off = narrative_append(buf, sizeof buf, off, ".");
    }

    if (self->origin_count > 0U) {
        off = narrative_append(buf, sizeof buf, off, " Origins:");
        for (size_t i = 0; i < self->origin_count; i++) {
            if (!self->origin_stories[i])
                continue;
            off = narrative_append(buf, sizeof buf, off, i == 0U ? " %s" : ", %s",
                                   self->origin_stories[i]);
        }
        off = narrative_append(buf, sizeof buf, off, ".");
    }

    if (self->current_preoccupation && self->current_preoccupation[0]) {
        off = narrative_append(buf, sizeof buf, off, " Current focus: %s.",
                               self->current_preoccupation);
    }

    off = narrative_append(buf, sizeof buf, off, "]");

    if (off >= sizeof buf) {
        hu_log_warn("narrative_self", NULL, "context truncated at %zu bytes", sizeof buf);
        buf[sizeof buf - 1] = '\0';
    }

    char *dup = hu_strndup(alloc, buf, strlen(buf));
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;

    *out = dup;
    *out_len = strlen(dup);
    return HU_OK;
}
