# Plan: Provider Dispatch Cleanup (Eliminate strcmp routing)

**Status:** Plan — well-bounded; could go to spec or directly to PR
**Author:** 2026-05-16 audit follow-up
**Owner:** TBD
**Risk:** Low — additive refactor with grep-style verification
**Effort:** 1 week

## Problem statement

The codebase advertises a vtable-driven provider architecture, but the audit
found three sites where dispatch happens via `strcmp` on provider name strings:

| Site | Count | Symptom |
|---|---|---|
| `src/onboard.c:275-371` | 15× `strcmp(provider, ...)` | Adding a provider requires editing onboard |
| `src/voice.c:58,101,337,435` | 4× hardcoded Cartesia check | New voice provider needs voice.c surgery |
| `src/config_merge.c:717-722` | provider defaults via string compare | Default-model logic outside the factory |

Net effect: **adding a new provider is not a "register in factory.c"
operation** — it requires changes in three other places. This is the opposite
of what the M5 thesis ("HuLa as Platform — Developer-facing SDK") promises.

## Goal

Adding a new provider should be a single-file change: implement
`hu_provider_vtable_t`, register in `src/providers/factory.c`. Done.

## Design

### Step 1 — Extend provider vtable

Add three optional methods to `hu_provider_vtable_t`:

```c
typedef struct hu_provider_vtable {
    // ... existing fields ...

    /* Onboarding hints — strings shown in `human onboard` for this provider.
       NULL → use defaults. */
    const hu_provider_onboard_hints_t *(*onboard_hints)(void);

    /* Default model ID for this provider. NULL → first model in supported_models. */
    const char *(*default_model)(void);

    /* Whether this provider is selectable in voice contexts. False by default. */
    bool (*supports_voice)(void);
} hu_provider_vtable_t;
```

All three are optional. Existing providers return NULL / defaults.

### Step 2 — Migrate onboard.c

Replace the 15 `strcmp` blocks with one loop:

```c
for (size_t i = 0; i < provider_count; i++) {
    const hu_provider_factory_entry_t *e = &provider_factories[i];
    if (e->vtable->onboard_hints) {
        const hu_provider_onboard_hints_t *h = e->vtable->onboard_hints();
        if (selected_provider == e->key) {
            apply_hints(h);
            break;
        }
    }
}
```

This is mechanical. Each existing `strcmp` block becomes the `onboard_hints`
return value for that provider's vtable.

### Step 3 — Migrate voice.c

Replace Cartesia hardcoded checks with `vtable->supports_voice()`. If a
provider doesn't define the method, it returns false — same as today's
"voice doesn't know this provider" path.

### Step 4 — Migrate config_merge.c

Replace `if (strcmp(provider, "openai") == 0) cfg->model = "gpt-4";` with
`cfg->model = vtable->default_model() ?? FALLBACK_MODEL;`.

### Step 5 — Verify

`grep -rn 'strcmp(provider' src/` should return empty (or only the factory
key lookup itself).

`grep -rn 'strcmp(.*"openai"\|strcmp(.*"anthropic"\|strcmp(.*"gemini"' src/`
should return zero hits outside `src/providers/`.

## Acceptance criteria

| AC | Description | Verification |
|---|---|---|
| AC-1 | `onboard.c` has zero `strcmp(provider, ...)` calls. | Grep. |
| AC-2 | `voice.c` has zero hardcoded provider name strings. | Grep. |
| AC-3 | `config_merge.c` has zero `strcmp(.*, "openai"\|"anthropic"\|"gemini"\|...)`. | Grep. |
| AC-4 | Adding a new provider requires changes to **only** `src/providers/<name>.c` and `src/providers/factory.c`. | Demonstrated by adding a no-op `dummy` provider as part of the PR. |
| AC-5 | All existing provider tests pass unchanged. | CI. |

## Out of scope

- Other strcmp-based dispatch elsewhere (channels, memory engines). Each gets
  its own follow-up if/when audited.
- Provider configuration UI rewrite. The onboard wizard's per-provider hints
  are now sourced from vtables, but the UI itself doesn't change.

## Audit evidence

- `src/onboard.c:275-371` — 15 `strcmp` provider checks.
- `src/voice.c:58,101,337,435` — Cartesia hardcoded.
- `src/config_merge.c:717-722` — provider default models hardcoded.

## Risks

- **Vtable bloat.** Adding three methods to every provider's vtable. *Mitigation:*
  all three are optional; providers that don't care return NULL.
- **Default model rot.** If a provider's `default_model()` returns a deprecated
  model ID, behavior changes. *Mitigation:* AC-5 catches this; also add a
  startup warning when a default model isn't in `supported_models`.
- **Onboarding wizard divergence.** If wizard logic was the source of truth
  for "what providers exist," now factory.c is. *Mitigation:* one-time scan
  to ensure factory has every provider the wizard knows about.
