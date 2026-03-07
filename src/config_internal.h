#ifndef SC_CONFIG_INTERNAL_H
#define SC_CONFIG_INTERNAL_H

#include "seaclaw/config.h"
#include "seaclaw/core/json.h"
#include "seaclaw/security.h"

#define SC_CONFIG_DIR        ".seaclaw"
#define SC_CONFIG_FILE       "config.json"
#define SC_DEFAULT_WORKSPACE "workspace"
#define SC_MAX_PATH          1024

const char *sc_config_sandbox_backend_to_string(sc_sandbox_backend_t b);
sc_sandbox_backend_t sc_config_parse_sandbox_backend(const char *s);
const char *sc_config_env_get(const char *name);
void sc_config_apply_env_str(sc_allocator_t *a, char **dst, const char *v);

#endif
