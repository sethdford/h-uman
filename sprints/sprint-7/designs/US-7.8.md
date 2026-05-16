# Design for US-7.8: MoLoRA static per-channel router — Init #02 phase 1

Sprint: 7 (sprint-7-digital-twin-dpo, base `13b89763`)
Risk tier: MEDIUM (new config keys, new include, conditional compile, touches inference dispatch path)
Gate: `HU_ENABLE_MOLORA` (CMake option, default OFF)
Out of scope: learned MLP router, differentiable routing (LD-MoLE/DynMoLE), training per-channel adapters, MLX provider.

## 1. Approach

Phase 1 is **static dispatch only**: a small router struct that maps a normalized channel id → adapter path, plus one hook in the llamacpp provider's `chat` entry that consults the router before each turn and calls the existing `load_adapter` path if the selected adapter differs from the currently-active one. There is no learned MLP, no message-class classifier, no scoring network — those land in later Init #02 phases. The router state is initialized once from `config.personalization.molora.channel_adapters` at provider construction and is read-only thereafter.

**Lazy swap, not pre-load.** Pre-loading 4–8 adapters at startup would cost roughly 4–8× the LoRA RAM footprint and add cold-load TTFT on `human` startup, with the worst case being a user who only ever uses Telegram paying for 7 unused adapters. Lazy swap costs one `llama_adapter_lora_init` + `llama_set_adapters_lora` per *channel transition* (not per turn — within a channel the active adapter is sticky). The existing `clear_active_adapter` + `llama_set_adapters_lora(ctx, NULL, 0, NULL)` semantics already handle "replace at-most-one adapter atomically" (see `src/providers/llamacpp.c:432-456`), and already invalidate the KV cache on swap (line 451). We reuse that contract — the router only chooses *which* path to load; the provider's existing vtable does the load.

**Where channel_id comes from.** `hu_agent_t.active_channel` + `active_channel_len` (declared in `include/human/agent.h:404-405`, set per turn by the channel-loop in `daemon.c` before invoking the agent). The provider does not see the agent struct directly — we pass channel id through the chat path by extending the existing `chat` call site in `src/agent/agent_turn.c` (around line 4784) to first call `hu_provider_load_adapter` with the router-selected path when `HU_ENABLE_MOLORA` is compiled in. This keeps `hu_provider_t`'s vtable shape unchanged (P0 stability: vtables are explicitly listed as out-of-scope in the sprint non-goals at `docs/plans/2026-05-11-init-02-molora-channels.md:269-270`).

## 2. Existing-code interface notes (load-bearing snippets)

- **llamacpp adapter vtable** (`src/providers/llamacpp.c`):
  - `llamacpp_load_adapter` (line 408) — already calls `llama_adapter_lora_init` + `llama_set_adapters_lora(ctx, adapters, 1, &scale)`, clears prior adapter, wipes KV cache. **This is exactly the API the M3 plan named — reuse, don't reinvent.**
  - `llamacpp_active_adapter` (line 490) — returns `c->active_adapter_id` or NULL. **Use this for the "is the right adapter already loaded?" idempotency check.**
  - `clear_active_adapter` (line 93) — internal helper that owns the `llama_set_adapters_lora(ctx, NULL, 0, NULL)` reset semantics.
- **Daemon adapter bootstrap** (`src/daemon.c:2455-2495`): currently loads exactly one adapter (`personalization.lora_adapter_path`) at startup. Under MoLoRA this becomes the *default/fallback* adapter; the router selects channel-specific overrides per turn. No behavior change when `molora.enabled = false`.
- **Agent channel context** (`include/human/agent.h:404`): `agent->active_channel` is a `const char *` (NOT null-terminated necessarily — paired with `active_channel_len`). The router's normalizer must take `(ptr, len)`, not a C string.
- **Chat dispatch site** (`src/agent/agent_turn.c:4784`): `agent->provider.vtable->chat(...)`. The hook lands immediately before this call (and before the degradation-fallback call at line 4799).
- **Personalization config block** (`include/human/config_types.h:69-75`): we extend `hu_personalization_config_t` with a nested `hu_molora_config_t` so the existing JSON path stays intact.
- **Parse / merge / serialize**: `src/config_parse.c::parse_personalization` (line 174), `src/config_merge.c:396`, `src/config_serialize.c:321`. All three must learn the new nested `molora` object.
- **CMake option pattern** (`CMakeLists.txt:7-40`): plain `option(HU_ENABLE_X "desc" OFF)` followed by a `target_compile_definitions(human PRIVATE HU_ENABLE_X=1)` guarded by `if(HU_ENABLE_X)`. Same shape as `HU_ENABLE_KV_COMPRESSION` (line 40).

## 3. File plan

| File | Change | Est LOC |
|---|---|---|
| `include/human/ml/molora.h` | **ADD** — `hu_molora_router_t`, `hu_molora_entry_t`, `hu_molora_router_init/select/free`, normalizer | +85 |
| `src/ml/molora.c` | **ADD** — router impl; conditionally compiled (CMake guard) | +180 |
| `include/human/config_types.h` | **MOD** — add `hu_molora_config_t { bool enabled; hu_molora_channel_entry_t entries[HU_MOLORA_MAX_CHANNELS]; size_t count; }` and a `molora` field on `hu_personalization_config_t` | +25 |
| `src/config_parse.c` | **MOD** — `parse_personalization` learns nested `"molora": {"enabled": bool, "channel_adapters": {chan: path,...}}` | +40 |
| `src/config_merge.c` | **MOD** — zero-init and free of `molora` entries | +20 |
| `src/config_serialize.c` | **MOD** — emit `molora` block when populated | +30 |
| `src/agent/agent_turn.c` | **MOD** — `#ifdef HU_ENABLE_MOLORA` block immediately before primary `vtable->chat` call: select adapter from router, call `hu_provider_load_adapter` if different from `active_adapter` | +35 |
| `CMakeLists.txt` | **MOD** — `option(HU_ENABLE_MOLORA …)`, compile-def, add `src/ml/molora.c` only when ON | +8 |
| `tests/test_molora_router.c` | **ADD** — 5 tests covering AC-7.8.1..4 and the disabled-path | +220 |
| `tests/CMakeLists.txt` | **MOD** — register new test under `HU_ENABLE_MOLORA` guard | +4 |
| `scripts/check-molora-binary-budget.sh` | **ADD** — size-delta check, called from CI for AC-7.8.5 | +35 |

Net: ~680 LOC across ~11 files. The router itself is ~265 LOC (header + impl); the rest is config plumbing and tests.

## 4. Public API sketch (header only — implementer fills in)

```c
/* include/human/ml/molora.h — Phase 1: static router (no learned MLP). */
#define HU_MOLORA_MAX_CHANNELS 16
#define HU_MOLORA_CHANNEL_NAME_MAX 32

typedef struct hu_molora_entry {
    char channel[HU_MOLORA_CHANNEL_NAME_MAX]; /* normalized lower-case prefix */
    const char *adapter_path;                 /* borrowed from config; not owned */
} hu_molora_entry_t;

typedef struct hu_molora_router {
    bool enabled;
    hu_molora_entry_t entries[HU_MOLORA_MAX_CHANNELS];
    size_t count;
    const char *default_adapter_path;         /* fallback; from personalization.lora_adapter_path */
} hu_molora_router_t;

/* Zero-init: a {0} struct is a valid disabled router. */
hu_error_t hu_molora_router_init(hu_molora_router_t *r, const hu_config_t *cfg);

/* Returns the adapter path for channel, or default_adapter_path, or NULL.
 * NEVER allocates. Safe to call on a disabled router (returns NULL). */
const char *hu_molora_router_select(const hu_molora_router_t *r,
                                    const char *channel, size_t channel_len);

/* Normalizes "telegram:42", "Telegram", "telegram " → "telegram".
 * Used internally by both init and select for case/prefix tolerance. */
size_t hu_molora_router_normalize_channel(const char *in, size_t in_len,
                                          char *out, size_t out_cap);
```

The struct is **zero-initializable** (AC-7.8.4): `(hu_molora_router_t){0}` represents the disabled state, and `hu_molora_router_select` on it returns NULL — caller falls through to today's behavior.

## 5. Implementation steps (for implementer agent)

1. **Header + CMake option.** Add `include/human/ml/molora.h` with declarations only. Add `option(HU_ENABLE_MOLORA "…" OFF)` and conditional `target_compile_definitions(human PRIVATE HU_ENABLE_MOLORA=1)` in `CMakeLists.txt`. Verify: `cmake --preset dev -DHU_ENABLE_MOLORA=ON && cmake --build --preset dev -- -j 4` builds (will fail link until step 2; commit step 1+2 together).
2. **Config types + parse/merge/serialize.** Extend `hu_personalization_config_t` with the `molora` block; teach parser to read `"molora": {"enabled": …, "channel_adapters": {…}}`; teach merge to zero-init and free; teach serializer to round-trip. Verify: `./build/human_tests --filter=config_parse` and `--filter=config_round_trip` still pass. Add one new fixture asserting parse round-trip of a `molora` block.
3. **Router impl skeleton.** `src/ml/molora.c` with `_init` (copies channel→path pointers from config), `_normalize_channel` (lowercase, strip at first `:`/space, bound to `HU_MOLORA_CHANNEL_NAME_MAX`), `_select` (linear scan over `entries[]`, returns first match or `default_adapter_path`). No allocations; entries hold borrowed pointers from `hu_personalization_config_t`. Verify: builds clean under both ON and OFF.
4. **Tests for router + normalizer.** Add `tests/test_molora_router.c`: cases for AC-7.8.1 (telegram → telegram-adapter), AC-7.8.2 (unknown channel → default), AC-7.8.3 (disabled router returns NULL — compile-only assertion via `#ifndef HU_ENABLE_MOLORA` block + a `static_assert(sizeof(hu_molora_router_t) > 0)`), AC-7.8.4 (zero-init is valid disabled state), plus normalizer table tests (`"Telegram"`, `"telegram:42"`, `"telegram "`, `""`, `"x"*40` all resolve to expected normalized forms). Verify: `./build/human_tests --filter=molora` all pass.
5. **Agent-turn hook.** In `src/agent/agent_turn.c` immediately before line 4784 (`agent->provider.vtable->chat(...)`), wrap a `#ifdef HU_ENABLE_MOLORA` block that: (a) calls `hu_molora_router_select(&agent->molora_router, agent->active_channel, agent->active_channel_len)`; (b) if the returned path is non-NULL AND differs from `agent->provider.vtable->active_adapter(agent->provider.ctx)` (when that vtable hook is present), calls `hu_provider_load_adapter` with a derived adapter id (basename of path, same convention as `daemon.c:2470`); (c) on `HU_ERR_NOT_SUPPORTED` logs once-per-turn and proceeds; on other errors logs warn and proceeds with base. The agent struct gains one `hu_molora_router_t molora_router;` field (zero-initialized by existing `calloc`); `hu_molora_router_init` is called from the same daemon bootstrap site that already calls `hu_provider_load_adapter` at `src/daemon.c:2467`, under `HU_ENABLE_MOLORA` guard. Verify: full suite `./build/human_tests` passes; new agent-turn test using mock provider in `test_molora_router.c` confirms the adapter-swap call happens with `channel="telegram"`.
6. **Sequencing lock with US-7.7 best-of-N.** Add a comment block at the agent-turn hook documenting: adapter selection happens **before** any best-of-N sampling loop (so all N candidates draw from the same channel-correct adapter). No code change needed unless US-7.7 merges first — then verify the loop's sampling site is below our hook insertion point.
7. **Binary size gate.** Add `scripts/check-molora-binary-budget.sh`: build `--preset release` twice (once with `-DHU_ENABLE_MOLORA=OFF`, once with `=ON`), `stat -f %z` (macOS) / `stat -c %s` (linux) on each `build/release/human`, assert `(on - off) <= 8192`. Document the command in the test plan section. Verify locally: run the script on a clean tree, observe delta.
8. **Disabled-path regression test.** Add `tests/test_molora_disabled_path.c` (or extend `test_molora_router.c`): when compiled with `HU_ENABLE_MOLORA` undefined, the symbol `hu_molora_router_select` is *not* linked into the agent path (use `nm build/human | grep molora` in a CI assertion, or simpler: `#ifdef HU_ENABLE_MOLORA` guard the test itself and rely on the OFF build's full test pass as the negative evidence). Verify: `cmake --preset dev -DHU_ENABLE_MOLORA=OFF && ./build/human_tests` passes without any molora symbols active.
9. **Doc + AC traceability.** Append a short paragraph to `docs/plans/2026-05-11-init-02-molora-channels.md` Phase-1 section linking to this design and the test file.

## 6. Test strategy (AC mapping)

| AC | Test | Mechanism |
|---|---|---|
| AC-7.8.1 | `test_static_router_selects_channel_adapter` | Configure `channel_adapters = {telegram: "/tmp/t.lora", slack: "/tmp/s.lora"}`, set `agent->active_channel = "telegram"`, mock provider's `load_adapter` to record `(path, id)` calls; assert recorded path ends with `t.lora` |
| AC-7.8.2 | `test_static_router_fallback_to_default` | Same config, set `active_channel = "irc"` (not in map), `default_adapter_path = "/tmp/d.lora"`; assert `load_adapter` is called with `d.lora`; no error logged |
| AC-7.8.3 | `test_disabled_molora_no_call` | `molora.enabled = false`; mock provider's `load_adapter` is a sentinel that asserts on entry; chat call must succeed. Plus a separate compile-only test: `#ifndef HU_ENABLE_MOLORA\n#error "OFF path must not include this TU"\n#endif` at top of `tests/test_molora_router.c` ensures the test only builds when the gate is ON |
| AC-7.8.4 | `test_router_zero_init_is_disabled` | `hu_molora_router_t r = {0}; assert(hu_molora_router_select(&r, "telegram", 8) == NULL);` |
| AC-7.8.5 | `scripts/check-molora-binary-budget.sh` (CI) | `cmake --preset release -DHU_ENABLE_MOLORA=OFF -B build/off`; `cmake --preset release -DHU_ENABLE_MOLORA=ON -B build/on`; `cmake --build build/off build/on`; compute `stat -c %s build/on/human - stat -c %s build/off/human`; fail if `> 8192` |

Normalizer table tests (not an AC but a risk-mitigation against the "channel-id is sometimes `telegram:42`" risk below): 8 cases, including the empty string, exact-match, `:`-suffix, ` `-suffix, mixed-case, and oversized inputs.

## 7. Risks (top 3, concrete)

1. **Cold-load latency × N adapters (MED probability / MED impact).** Per the M3 bridge plan, `llama_adapter_lora_init` parses the entire `.lora` file at load time (no mmap caching by default in our build). A user with `channel_adapters = {telegram, slack, discord, imessage}` who alternates channels every turn will pay 1× LoRA load (~50–200 ms for a typical 8-rank adapter on a 7B model on Apple silicon, per the M3 plan's calibration) per channel transition. **Mitigation:** ship lazy-swap with an LRU of 1 (only the currently active adapter resident) for Phase 1; document the latency cost in the persona-onboarding wizard; defer the multi-resident cache to Phase 2 once we have real DAU data on channel-switch frequency. Idempotency check via `active_adapter()` ensures intra-channel turns pay zero swap cost.
2. **Channel-id normalization edge cases (HIGH probability / SMALL impact).** `agent->active_channel` is populated by ~31 different channel modules. Telegram sets it to `"telegram"`; iMessage sets it to `"imessage"` *or* `"imessage:<chat_id>"` (the colon-suffix form is used by `daemon_proactive.c`); Slack sets it to `"slack"` plus the channel-id list lives elsewhere. The router would silently miss matches if we did string-equal. **Mitigation:** normalizer lower-cases and truncates at the first `:` or whitespace. Eight enumerated normalizer tests above. If a channel module ever populates with a wholly different prefix (e.g. WhatsApp Business uses `"wabiz"`), the fallback path covers it without error.
3. **Interaction with US-7.7 best-of-N adapter sampling (MED / MED).** US-7.7 (also in this sprint) samples N candidates per turn. If best-of-N's loop *also* loaded adapters (it doesn't today, but the design is open) or if it ran *outside* the agent-turn hook, the N candidates could draw from inconsistent adapters. **Mitigation locked here:** the agent-turn hook lands at the single chat dispatch site (line 4784), and best-of-N is implemented inside the provider's chat method (per US-7.7 design). The order is therefore: (1) MoLoRA picks adapter → (2) chat() is called → (3) inside chat, N candidates are sampled. All N share the same adapter. The implementer must verify this sequencing is preserved if US-7.7 lands first; the design doc for US-7.7 (`sprints/sprint-7/designs/US-7.7.md`) needs cross-referencing.

Out-of-band fourth risk worth noting (not in top-3 because mitigated structurally): **vtable stability** — the sprint plan calls out that vtables are out of scope. Our design adds zero vtable methods; we only call existing `load_adapter` / `active_adapter` hooks. No ABI risk.

## 8. Sequencing notes for the implementer

- **Don't pre-load adapters at init.** Lazy swap is the contract. If a future story changes this, it's a new design doc.
- **Don't add a new vtable method** on `hu_provider_t`. Use the existing `load_adapter` + `active_adapter`. The router lives outside the provider.
- **Don't allocate inside `hu_molora_router_select`.** It is called on every turn. The router owns no memory; it borrows from the config.
- **Idempotency.** If `active_adapter()` returns the id matching the router's selection, *skip* the load call. This is the only thing standing between us and "pay 100 ms swap on every turn even though the channel hasn't changed."
- **OFF-build behavior.** With `HU_ENABLE_MOLORA=OFF`, neither `molora.h` nor `molora.c` is compiled into `human`; the `#ifdef`-guarded hook in `agent_turn.c` is preprocessor-elided; binary is byte-identical to a pre-story build modulo any new config-parser code paths that *parse* the molora block silently (still under the OFF flag for the parser too, to be safe).

## 9. Open questions (require sprint-owner / product-owner sign-off if blocked)

- **Q1**: Should `molora.channel_adapters` be a flat map (`{telegram: "...", slack: "..."}`) or nested objects (`{telegram: {path: "...", scale: 0.8}}`)? Phase 1 plan says flat map; Phase 2 will want scale. Recommend flat for now and break compat at Phase 2 with a versioned schema bump. **Decision needed: confirm flat map.** (Default: flat; non-blocking — Phase 2 can extend.)
- **Q2**: When `personalization.enabled = false` but `molora.enabled = true`, do we honor molora? Recommend: **no** — molora is a sub-feature of personalization; if the parent is off, the router stays disabled. Mirrors how persona overlays gate on persona.enabled. (Non-blocking; documented in the parser.)
- **Q3**: Does `human doctor` warn on a configured `channel_adapters` entry pointing at a missing file? Recommend: yes, add a single-line doctor check (separate story, not blocking US-7.8). Captured as a DEBT- task for the sprint board.

None of the open questions are blocking. Recommend `READY` with Q1 default.

---

RESULT_tech-lead=READY
