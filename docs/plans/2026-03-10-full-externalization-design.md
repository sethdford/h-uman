# Full Codebase Externalization Design

**Goal:** Remove all hardcoded strings, word lists, prompt templates, thresholds, and paths from C code. Embed defaults at compile time via `xxd`, allow runtime overrides from `~/.human/data/`.

**Architecture:** Compile-time embedding with runtime override. Single `hu_data_load()` function checks filesystem first, falls back to embedded byte arrays.

## Data Directory Structure

```
data/
├── prompts/
│   ├── safety_rules.txt
│   ├── autonomy_readonly.txt
│   ├── autonomy_supervised.txt
│   ├── autonomy_full.txt
│   ├── persona_reinforcement.txt
│   ├── session_startup.txt
│   └── group_chat_hint.txt
├── conversation/
│   ├── ai_phrases.json
│   ├── filler_words.json
│   ├── contractions.json
│   ├── conversation_intros.json
│   └── ai_disclosure_patterns.json
├── persona/
│   ├── circadian_phases.json
│   └── mood_vocabularies.json
├── security/
│   └── command_lists.json
└── tone/
    └── tone_keywords.json
```

## Core Components

### 1. hu_data_loader

New module: `src/data/loader.c`, `include/human/data/loader.h`

```c
hu_error_t hu_data_load(hu_allocator_t *alloc, const char *relative_path,
                        char **out, size_t *out_len);
```

- Checks `~/.human/data/<path>` (or config `data_dir`)
- Falls back to compiled-in embedded default
- Returns allocated copy (caller owns)

### 2. Build System

CMake custom command: `xxd -i` each file in `data/` → `src/data/embedded_*.c`
Registry: `src/data/embedded_registry.c` with path → {data, len} lookup table.
Auto-regenerates on data file changes.

### 3. Config Additions

- `temp_dir` — overrides `hu_platform_get_temp_dir()`
- `data_dir` — overrides `~/.human/data/`
- Per-channel `max_message_size`
- Thresholds: `consecutive_limit`, `participation_pct`, `callback_window`, `pattern_threshold`, `tapback_skip_pct`

### 4. Files Touched

~50 files across src/, include/, tests/, data/, CMakeLists.txt
