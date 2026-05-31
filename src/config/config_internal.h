#ifndef HU_CONFIG_INTERNAL_H
#define HU_CONFIG_INTERNAL_H

#include "human/config.h"
#include "human/core/json.h"
#include "human/security.h"

#define HU_CONFIG_DIR        ".human"
#define HU_CONFIG_FILE       "config.json"
#define HU_DEFAULT_WORKSPACE "workspace"
#define HU_MAX_PATH          1024

const char *hu_config_sandbox_backend_to_string(hu_sandbox_backend_t b);
hu_sandbox_backend_t hu_config_parse_sandbox_backend(const char *s);
const char *hu_config_env_get(const char *name);
void hu_config_apply_env_str(hu_allocator_t *a, char **dst, const char *v);

#endif
