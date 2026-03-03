---
title: Config API
description: Configuration loading, env overrides, and schema validation
updated: 2026-03-02
---

# Config API

Configuration loading from `~/.seaclaw/config.json`, env overrides, and schema validation.

## Types (Selected)

```c
typedef struct sc_config {
    char *workspace_dir;
    char *config_path;
    char *workspace_dir_override;
    char *api_key;
    sc_provider_entry_t *providers;
    size_t providers_len;
    char *default_provider;
    char *default_model;
    double default_temperature;
    uint32_t max_tokens;
    char *memory_backend;
    bool memory_auto_save;
    bool heartbeat_enabled;
    uint32_t heartbeat_interval_minutes;
    char *gateway_host;
    uint16_t gateway_port;
    bool workspace_only;
    uint32_t max_actions_per_hour;
    sc_diagnostics_config_t diagnostics;
    sc_autonomy_config_t autonomy;
    sc_runtime_config_t runtime;
    sc_reliability_config_t reliability;
    sc_agent_config_t agent;
    sc_channels_config_t channels;
    sc_memory_config_t memory;
    sc_config_gateway_t gateway;
    sc_tools_config_t tools;
    /* ... more fields */
    sc_arena_t *arena;
    sc_allocator_t allocator;
} sc_config_t;

typedef struct sc_provider_entry {
    char *name;
    char *api_key;
    char *base_url;
    bool native_tools;
} sc_provider_entry_t;
```

## Key Functions

```c
sc_error_t sc_config_load(sc_allocator_t *backing, sc_config_t *out);
void sc_config_deinit(sc_config_t *cfg);
sc_error_t sc_config_parse_json(sc_config_t *cfg, const char *content, size_t len);
void sc_config_apply_env_overrides(sc_config_t *cfg);
sc_error_t sc_config_save(const sc_config_t *cfg);
sc_error_t sc_config_validate(const sc_config_t *cfg);
```

## Provider Lookup

```c
const char *sc_config_get_provider_key(const sc_config_t *cfg, const char *name);
const char *sc_config_default_provider_key(const sc_config_t *cfg);
bool sc_config_provider_requires_api_key(const char *provider);
const char *sc_config_get_provider_base_url(const sc_config_t *cfg, const char *name);
bool sc_config_get_provider_native_tools(const sc_config_t *cfg, const char *name);
```

## Usage Example

```c
sc_allocator_t alloc = sc_system_allocator();
sc_config_t cfg;
sc_error_t err = sc_config_load(&alloc, &cfg);
if (err != SC_OK) { /* handle */ }

sc_config_apply_env_overrides(&cfg);

const char *key = sc_config_get_provider_key(&cfg, "openai");
const char *model = cfg.default_model;

/* use cfg ... */

sc_config_deinit(&cfg);
```

## Schema

Config file location: `~/.seaclaw/config.json` (or path in `SC_CONFIG_PATH`). See project docs for full schema.
