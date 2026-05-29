---
title: Config API
description: Configuration loading, env overrides, and schema validation
updated: 2026-03-02
---

# Config API

Configuration loading from `~/.human/config.json`, env overrides, and schema validation.

## Types (Selected)

```c
typedef struct hu_config {
    char *workspace_dir;
    char *config_path;
    char *workspace_dir_override;
    char *api_key;
    hu_provider_entry_t *providers;
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
    hu_diagnostics_config_t diagnostics;
    hu_autonomy_config_t autonomy;
    hu_runtime_config_t runtime;
    hu_reliability_config_t reliability;
    hu_agent_config_t agent;
    hu_channels_config_t channels;
    hu_memory_config_t memory;
    hu_config_gateway_t gateway;
    hu_tools_config_t tools;
    /* ... more fields */
    hu_arena_t *arena;
    hu_allocator_t allocator;
} hu_config_t;

typedef struct hu_provider_entry {
    char *name;
    char *api_key;
    char *base_url;
    bool native_tools;
} hu_provider_entry_t;
```

## Key Functions

```c
hu_error_t hu_config_load(hu_allocator_t *backing, hu_config_t *out);
void hu_config_deinit(hu_config_t *cfg);
hu_error_t hu_config_parse_json(hu_config_t *cfg, const char *content, size_t len);
void hu_config_apply_env_overrides(hu_config_t *cfg);
hu_error_t hu_config_save(const hu_config_t *cfg);
hu_error_t hu_config_validate(const hu_config_t *cfg);
```

## Provider Lookup

```c
const char *hu_config_get_provider_key(const hu_config_t *cfg, const char *name);
const char *hu_config_default_provider_key(const hu_config_t *cfg);
bool hu_config_provider_requires_api_key(const char *provider);
const char *hu_config_get_provider_base_url(const hu_config_t *cfg, const char *name);
bool hu_config_get_provider_native_tools(const hu_config_t *cfg, const char *name);
```

Provider keys are also used for optional integrations beyond the model
provider, looked up by name via `hu_config_get_provider_key(cfg, "<name>")`:

| Key name   | Enables |
|------------|---------|
| `spotify`  | Spotify share links for music inspirations (`client_id:client_secret`) |
| `tenor`    | Tenor GIF search |
| `youtube`  | YouTube video inspirations (YouTube Data API v3 key). When absent, YouTube inspirations are silently skipped and the picker falls back to music. |

TikTok inspirations need no key — they resolve to public hashtag
discovery URLs (`tiktok.com/tag/<keyword>`).

## Usage Example

```c
hu_allocator_t alloc = hu_system_allocator();
hu_config_t cfg;
hu_error_t err = hu_config_load(&alloc, &cfg);
if (err != HU_OK) { /* handle */ }

hu_config_apply_env_overrides(&cfg);

const char *key = hu_config_get_provider_key(&cfg, "openai");
const char *model = cfg.default_model;

/* use cfg ... */

hu_config_deinit(&cfg);
```

## Schema

Config file location: `~/.human/config.json` (or path in `HU_CONFIG_PATH`). See project docs for full schema.

### `agent.hula` (boolean)

When **true** (default after config merge), the agent may execute **HuLa** JSON programs: structured `seq` / `par` / `branch` / `loop` / `delegate` / `emit` plans over tools, with policy checks and optional trace files. The system prompt also includes the `<hula_program>...</hula_program>` convention when HuLa is enabled.

Set `"hula": false` to disable HuLa paths and rely on the standard tool dispatcher only.

**Guide:** [`docs/guides/hula.md`](../guides/hula.md).

### `agent.metacognition` (object)

Optional. Parsed into `hu_metacog_settings_t` and applied to the agent after `hu_agent_from_config` (bootstrap, CLI, spawns with parent policy, subagent tasks).

| Key | Type | Description |
| --- | --- | --- |
| `enabled` | bool | Master switch for metacognition loop, SQLite `metacog_history`, follow-up outcome labeling, difficulty/BTH counters. |
| `confidence_threshold` | number 0–1 | Below this (after difficulty scaling), plan may choose REFLECT / CLARIFY. |
| `coherence_threshold` | number 0–1 | Below → SWITCH_STRATEGY. |
| `repetition_threshold` | number 0–1 | Above → SIMPLIFY (with stuck). |
| `max_reflects` | int | Cap REFLECT before CLARIFY. |
| `max_regen` | int | Max provider re-calls per assistant turn (bounded re-entry). |
| `hysteresis_min` | int | Consecutive “bad” signals before a costly action fires. |
| `use_calibrated_risk` | bool | Weighted risk score can reduce hysteresis when high. |
| `risk_high_threshold` | number 0–1 | Threshold for that reduction. |
| `w_low_confidence` … `w_low_trajectory` | number | Non-negative weights for calibrated risk (normalized internally). |

**Environment overrides** (see `hu_config_apply_env_overrides`): `HUMAN_METACOGNITION`, `HUMAN_METACOG_MAX_REGEN`. **`HUMAN_METACOG_LOGPROBS`** is read in the agent turn when metacognition is enabled (requests logprobs from OpenAI-style APIs).

**Operational queries** on `~/.human/cognition.db` → `metacog_history`: see `docs/operations/metacog-analytics.md`.
