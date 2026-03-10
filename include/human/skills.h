#ifndef HU_SKILLS_H
#define HU_SKILLS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/skillforge.h"
#include <stddef.h>

hu_error_t hu_skills_list(hu_allocator_t *alloc, const char *workspace_dir, hu_skill_t **out_skills,
                          size_t *out_count);

void hu_skills_free(hu_allocator_t *alloc, hu_skill_t *skills, size_t count);

#endif /* HU_SKILLS_H */
