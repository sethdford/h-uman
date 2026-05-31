---
title: Configuration Schema Standard
---

# Configuration Schema Standard

Standards for designing, parsing, and validating configuration in human.

**Cross-references:** [api-design.md](api-design.md), [principles.md](principles.md), [../security/threat-model.md](../security/threat-model.md)

---

## Overview

The configuration system drives agent behavior, providers, channels, security policies, and runtime parameters. This standard ensures consistent schema design, validation, and upgrade paths.

**Related code:** `src/config/config_parse.c`, `include/human/config.h`, `include/human/config_types.h`.

---

## Configuration Structure Hierarchy

### Root: `hu_config_t`

Top-level configuration struct (`include/human/config.h`):

```c
typedef struct hu_config {
    // Agent behavior
    hu_agent_config_t agent;
    
    // Providers (AI models)
    hu_provider_entry_t *providers;
    size_t providers_len;
    
    // Channels (messaging)
    hu_channel_entry_t *channels;
    size_t channels_len;
    
    // Security
    hu_policy_config_t policy;
    hu_security_config_t security;
    
    // Storage/Memory
    hu_memory_config_t memory;
    
    // Runtime
    hu_runtime_config_t runtime;
    
    // Reliability
    hu_reliability_config_t reliability;
    
    // Diagnostics
    hu_diagnostics_config_t diagnostics;
} hu_config_t;
```

### Nested Contexts

Each subsystem has its own config struct that must be self-contained (no circular references):

| Context | Struct | File | Responsibility |
|---------|--------|------|-----------------|
| Agent | `hu_agent_config_t` | config.h | LLM behavior, tools, memory |
| Provider | `hu_provider_entry_t` | config.h | AI model endpoint + credentials |
| Channel | `hu_channel_entry_t` | config.h | Messaging channel config |
| Policy | `hu_policy_config_t` | config.h | Security rules, autonomy |
| Memory | `hu_memory_config_t` | config.h | Storage backend, consolidation |
| Runtime | `hu_runtime_config_t` | config.h | Execution environment (native, Docker, etc.) |

---

## JSON Configuration File Format

Configuration is loaded from `~/.human/config.json` (or `$HU_CONFIG_FILE`):

```json
{
  "agent": {
    "persona": "helpful-assistant",
    "max_tool_iterations": 5,
    "llm_compiler_enabled": true,
    "token_limit": 8000
  },
  "providers": [
    {
      "name": "anthropic",
      "api_key": "${HU_ANTHROPIC_API_KEY}",
      "model": "claude-opus-4-6"
    },
    {
      "name": "openai",
      "api_key": "${HU_OPENAI_API_KEY}",
      "model": "gpt-4"
    },
    {
      "name": "apple",
      "base_url": "http://127.0.0.1:11435/v1",
      "model": "apple-foundationmodel"
    }
  ],
  "channels": [
    {
      "name": "cli",
      "type": "cli"
    },
    {
      "name": "slack",
      "type": "slack",
      "auth_token": "${HU_SLACK_BOT_TOKEN}"
    }
  ],
  "policy": {
    "enabled": true,
    "autonomy_level": "supervised",
    "allowed_commands": ["git", "ls", "find"]
  },
  "memory": {
    "backend": "sqlite",
    "database_path": "${HOME}/.human/memory.db"
  }
}
```

---

## Required vs Optional Fields

### Field Classification

| Level | Behavior | Example |
|-------|----------|---------|
| **Required** | Must be present; error if missing | `agent.persona` |
| **Optional with default** | Omit → use hardcoded default | `agent.max_tool_iterations` (default 5) |
| **Optional (NULL ok)** | May be present or absent | `memory.backend` (default NULL = no persistence) |

### Providing Defaults

1. **Compile-time defaults:** Use macro constants (`#define HU_AGENT_DEFAULT_AUTONOMY "supervised"`)
2. **Parse-time defaults:** Apply during `hu_config_parse()` if field is absent
3. **Runtime defaults:** Apply when config is first used (e.g., `if (config->memory.backend == NULL) use sqlite`)

**Example defaults in agent.c:**

```c
if (config->agent.max_tool_iterations == 0) {
    config->agent.max_tool_iterations = HU_AGENT_DEFAULT_MAX_TOOL_ITERATIONS;
}
```

---

## Environment Variable Interpolation

Config supports `${VAR_NAME}` syntax for environment variables:

```json
{
  "providers": [
    {
      "api_key": "${HU_OPENAI_API_KEY}",
      "base_url": "${HU_OPENAI_BASE_URL}"
    }
  ]
}
```

### Interpolation Rules

1. **Syntax:** `${VAR_NAME}` only; `$VAR_NAME` not supported
2. **Required vars:** If referenced but unset → `HU_ERR_INVALID_PARAM` with message "`${VAR_NAME}` not set"
3. **Scope:** Applies to string values only (not numbers, booleans)
4. **Nesting:** Do not nest `${${...}}` (not supported)
5. **Security:** Never log interpolated values; log placeholder instead (e.g., "`${HU_API_KEY}` (hidden)`")

**Validation:**

```c
// Parse time: check all ${...} variables are set
hu_error_t hu_config_validate_env_vars(const hu_config_t *config) {
    // Scan for ${...} patterns; fail if variable not set
}
```

---

## Policy Configuration (Security Policies)

Policy is stored in a separate JSON file (`~/.human/policy.json`) or nested in config.json:

```json
{
  "policy": {
    "enabled": true,
    "autonomy_level": "supervised",
    "allowed_commands": ["git", "npm", "python"],
    "blocked_commands": ["rm", "dd"],
    "allowed_paths": ["/home/user/projects", "/tmp"],
    "allowed_urls": ["github.com", "npmjs.com"],
    "blocked_urls": ["crypto-mining-pool.com"],
    "rate_limits": {
      "tools_per_minute": 10,
      "api_calls_per_hour": 100
    }
  }
}
```

### Policy Enforcement

- **Autonomy levels:** `supervised`, `assisted`, `autonomous` (default `supervised`)
- **Command allowlist:** If set, only commands in list are executable
- **Command blocklist:** If set, commands in list are forbidden (allowlist takes precedence)
- **Path allowlist:** If set, tool operations limited to these paths
- **URL blocklist:** If set, URLs matching patterns are rejected by http_request tool

See `src/security/policy.c` for enforcement logic.

---

## Migration: Adding New Fields

When adding a new field to config:

### 1. Add to Header

```c
// include/human/agent_config.h (or config.h)
typedef struct hu_agent_config {
    // ... existing fields ...
    
    // NEW FIELD
    bool new_feature_enabled;
} hu_agent_config_t;
```

### 2. Set Parse-Time Default

```c
// src/config/config_parse.c
hu_error_t hu_config_parse_json(const hu_json_object_t *json, hu_config_t *out) {
    // ... existing parsing ...
    
    // NEW FIELD — apply default if not in JSON
    hu_json_value_t *val = hu_json_object_get(json, "new_feature_enabled");
    if (val && hu_json_is_bool(val)) {
        out->agent.new_feature_enabled = hu_json_get_bool(val);
    } else {
        out->agent.new_feature_enabled = HU_NEW_FEATURE_DEFAULT; // default to false
    }
}
```

### 3. Maintain Backward Compatibility

- Old configs without the field must not error
- Use default value if field is absent
- Old config files continue to work unchanged

### 4. Add Test

```c
void config_parse_new_feature_defaults_when_absent(void) {
    hu_json_object_t *json = hu_json_object_new(alloc);
    // Do not set "new_feature_enabled"
    
    hu_config_t config;
    hu_error_t err = hu_config_parse_json(json, &config);
    
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(config.agent.new_feature_enabled, HU_NEW_FEATURE_DEFAULT);
}
```

---

## Validation Rules

### Parse-Time Validation

Run during `hu_config_parse()`:

1. **Type correctness:** string/number/bool match expected type
2. **Required field presence:** all required fields present
3. **Range validation:** numeric ranges (e.g., `max_retries` >= 1)
4. **Enum validation:** string values match allowed set
5. **File existence:** paths exist if specified
6. **Format validation:** email addresses, URLs match patterns

**Example:**

```c
// Validate autonomy_level is one of the allowed values
const char *level = config->policy.autonomy_level;
if (level && strcmp(level, "supervised") != 0 && 
            strcmp(level, "assisted") != 0 &&
            strcmp(level, "autonomous") != 0) {
    return hu_error_create(HU_ERR_INVALID_PARAM, 
        "autonomy_level must be 'supervised', 'assisted', or 'autonomous'");
}
```

### Runtime Validation

Check at agent startup:

1. **Provider availability:** named providers exist and can authenticate
2. **Channel availability:** named channels are configured
3. **Policy conflict detection:** e.g., tool in both allowed and blocked lists
4. **Workspace path:** configured workspace exists and is readable/writable

---

## Secrets Management

Secrets (API keys, tokens) should never be stored in plain text in config files.

### Recommended Pattern

1. **Reference by name in config:** `"api_key": "${HU_ANTHROPIC_API_KEY}"`
2. **Load from environment:** User sets `HU_ANTHROPIC_API_KEY` before starting
3. **Optional encrypted store:** `hu_secret_store_t` in `src/security/secrets.c` (ChaCha20+HMAC)

**Never:**
- Log unmasked API keys
- Store keys in version control (use `.gitignore` for config)
- Embed keys as defaults in source code

---

## Testing Expectations

Configuration must pass test suite:

```bash
./human_tests --suite=Config        # Config parsing
./human_tests --suite=ConfigPolicy  # Policy validation
```

Required tests:

- Parsing valid JSON with all required fields
- Parsing minimal JSON (only required fields)
- Parsing JSON with optional fields overriding defaults
- Env var interpolation (set and unset variables)
- Invalid JSON (malformed, missing required fields)
- Type mismatches (string where number expected)
- Policy validation (conflicts, invalid values)
- File I/O (read from path, handle missing file)

---

## Example: Complete Config

```json
{
  "agent": {
    "persona": "helpful-coder",
    "llm_compiler_enabled": true,
    "mcts_planner_enabled": false,
    "max_tool_iterations": 8,
    "max_history_messages": 50,
    "context_pressure_warn": 0.85,
    "context_pressure_compact": 0.95
  },
  "providers": [
    {
      "name": "anthropic",
      "api_key": "${HU_ANTHROPIC_API_KEY}",
      "model": "claude-opus-4-6"
    },
    {
      "name": "openai",
      "api_key": "${HU_OPENAI_API_KEY}",
      "model": "gpt-4"
    }
  ],
  "channels": [
    { "name": "cli", "type": "cli" },
    {
      "name": "slack",
      "type": "slack",
      "auth_token": "${HU_SLACK_BOT_TOKEN}",
      "signing_secret": "${HU_SLACK_SIGNING_SECRET}"
    }
  ],
  "policy": {
    "enabled": true,
    "autonomy_level": "supervised",
    "allowed_commands": ["git", "npm", "cargo"],
    "allowed_paths": ["${HOME}/projects"],
    "rate_limits": { "tools_per_minute": 10 }
  },
  "memory": {
    "backend": "sqlite",
    "database_path": "${HOME}/.human/memory.db"
  },
  "runtime": {
    "kind": "native"
  }
}
```

---

## Anti-Patterns

```
WRONG -- Hardcode secrets in config.json
RIGHT -- Use ${HU_SECRET_NAME} and set environment variable

WRONG -- Log interpolated values including secrets
RIGHT -- Log placeholder: "${HU_API_KEY} (hidden)"

WRONG -- Make new field required retroactively
RIGHT -- Add with default; existing configs continue to work

WRONG -- Store policy in config.json without versioning
RIGHT -- Use separate policy.json or include version field

WRONG -- Skip validation on custom fields
RIGHT -- Validate all config values before use
```

---

## Key Paths

- Config header: `include/human/config.h`
- Config types: `include/human/config_types.h`
- Parser: `src/config/config_parse.c`
- Policy enforcement: `src/security/policy.c`
- Tests: `tests/test_config_parse.c`, `tests/test_policy.c`
