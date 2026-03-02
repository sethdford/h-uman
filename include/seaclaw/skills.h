#ifndef SC_SKILLS_H
#define SC_SKILLS_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/skillforge.h"
#include <stddef.h>

sc_error_t sc_skills_list(sc_allocator_t *alloc, const char *workspace_dir,
    sc_skill_t **out_skills, size_t *out_count);

void sc_skills_free(sc_allocator_t *alloc, sc_skill_t *skills, size_t count);

#endif /* SC_SKILLS_H */
