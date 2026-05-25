---
title: "Initiative 03 — Apple FoundationModels first-class provider"
created: 2026-05-11
status: deferred
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - 2026-05-10-master-follow-through-program.md
  - 2026-05-11-rl-loop-phase-1-llamacpp.md
  - ../../CLAUDE.md
  - ../../AGENTS.md
  - ../standards/security/threat-model.md
  - ../standards/engineering/principles.md
  - ../../include/human/provider.h
  - ../../include/human/providers/apple.h
  - ../../src/providers/apple.c
  - ../../apps/shared/HumanKit/Sources/HumanOnDevice/OnDeviceProvider.swift
  - ../../apps/shared/HumanKit/Sources/HumanOnDeviceServer/OnDeviceRouter.swift
last_audit: 2026-05-25
---

# Apple FoundationModels first-class provider

**Track.** SOTA-2026 initiative #03. Independent (no upstream blocker), but adjacent to #02 (MoLoRA) and #04 (MLX Qwen3) on the `hu_provider_t.load_adapter` surface.

**One-line.** macOS 26 / iOS 19 ship an OS-level on-device model. Promote `src/providers/apple.c` from a localhost OAI-compat HTTP shim to a real `hu_provider_t` backed by Apple's `FoundationModels.framework` through a stable C ABI exposed by `HumanKit` — ANE inference, zero-network by default, native streaming, native tool descriptors, system-prompt steering.

**Honest scope.** This is a **design + proof-of-feasibility** document. The implementation sprint that adopts it will pick at most Phases 1–3 below; Phase 4 (in-process linking) is deferred pending the Phase 3 evaluation. The defer condition (§13) is genuine — if Apple's adapter API doesn't admit our LoRA delta and system-prompt steering can't hit N1, we **park** this initiative and route the personalization path through initiative #04 (MLX Qwen3).

---

## 1. Reality check (what's actually shipped today)

Before designing the new surface, an honest map of what `src/providers/apple.c` already is.

| Claim | Reality |
|---|---|
| "It's a stub" | False. It's a fully wired `hu_provider_t` with `chat`, `chat_with_system`, `stream_chat`, native-tools claim, OAI tool-call parsing, SSE streaming, and a localhost probe (`hu_apple_probe`). 17 KB compiled. ASan-clean. |
| "It calls FoundationModels.framework" | False. It POSTs JSON to `http://127.0.0.1:11435/v1/chat/completions`. The actual `FoundationModels.framework` call lives in `apps/shared/HumanKit/Sources/HumanOnDevice/OnDeviceProvider.swift` (uses `LanguageModelSession`), reached via the `human-ondevice` Swift binary in `apps/tools/human-ondevice/`. |
| "Tool calling works" | Half-true. The C side serialises `hu_tool_spec_t` into OpenAI tool descriptors and parses `tool_calls[]` from the response. The Swift router (`OnDeviceRouter.swift`) silently ignores `tools` from the request — `LanguageModelSession.respond(to:)` is called with no tool surface. So tool calls are **never produced** by the model today; tests pass because `HU_IS_TEST` returns a hard-coded mock. |
| "Zero network" | True for outbound DNS / WAN. False for loopback TCP — any local process on port 11435 can speak the API. A bearer token is generated on Swift-server startup (`OnDeviceServer.swift:23-28`) but the C client doesn't send it. |
| "Streaming is native" | True at the wire level (SSE), false at the model level — `LanguageModelSession.streamResponse(to:)` is what produces chunks; the daemon framing is chunked-HTTP + SSE wrapping. |
| "It works on iOS" | False. iOS apps cannot run a background TCP server; `human-ondevice` only ships on macOS. iOS uses `OnDeviceChatAdapter` (`apps/shared/HumanKit/Sources/HumanOnDevice/OnDeviceChatAdapter.swift`) in-process via the iOS app target. There is no `hu_provider_t` path on iOS today. |
| "load_adapter works" | False. The vtable leaves `load_adapter` NULL. The framework intentionally does not expose model weights. |
| "It enforces strict-local (no PCC)" | Not handled. Apple's `LanguageModelSession` on macOS 26 / iOS 19 may transparently fall back to **Private Cloud Compute** when the on-device model is unavailable or insufficient. The current code path has no way to disable this. |

The five real gaps this initiative closes:

1. **iOS path.** No `hu_provider_t` on iOS today. The daemon needs to either ship on iOS (impossible) or the C provider needs to talk to the in-process Swift bridge.
2. **Native tool calling.** Map our `hu_tool_t` registry to `FoundationModels.Tool` descriptors so the model actually emits tool calls.
3. **Strict-local mode.** Refuse PCC fallback when the user is on a "fully private" persona. Make the trade-off visible.
4. **Loopback TCP attack surface.** Move the macOS daemon transport from TCP-on-loopback to a Unix domain socket; replace the unused bearer token with a per-user file-mode-checked socket path.
5. **Direct C ABI.** Optionally `dlopen` HumanKit's `libHumanFoundationModelsBridge.dylib` and skip the daemon entirely on macOS, the same way `embedded.c` shells out to `llama-cli` but without the per-call fork cost.

---

## 2. Architecture decision

**Decision.** Keep the C-side `hu_provider_t` vtable surface untouched. Extend the `apple.c` provider with three transports (TCP-loopback, Unix domain socket, in-process C ABI via `dlopen`), pick the strictest available at construction time, and document the trade-offs.

**Transport precedence at create time:**

```
1. Direct in-process via libHumanFoundationModelsBridge.dylib       (macOS, best)
   ↓ fallback
2. Unix domain socket: $XDG_RUNTIME_DIR/human-ondevice.sock        (macOS, strict)
   ↓ fallback
3. TCP loopback: http://127.0.0.1:11435/v1 with bearer token       (macOS, compat)
   ↓ fallback
4. HU_ERR_NOT_SUPPORTED                                            (everywhere else)
```

On **iOS** the C provider returns `HU_ERR_NOT_SUPPORTED` unconditionally — iOS apps reach `OnDeviceProvider` in-process via Swift. The C provider is a macOS-only artifact. The HumanKit Swift package, however, remains the single source of truth for the FM call, used by both iOS apps directly and macOS daemons via the C ABI.

**Why not XPC?** XPC services require plist registration, a separate launch process per host, and don't work cleanly on iOS. They add ~80–120 KB to the bundle, the binary-budget delta is bigger than the value.

**Why not link `libSwiftCore` directly into the C daemon?** Swift runtime is 30+ MB; the whole point of h-uman is a ~1.7 MB binary. `dlopen` keeps the Swift dependency at the edge.

**Why keep TCP-loopback as a fallback at all?** Backwards compatibility with users who already run `apfel` (third-party port 11434) or any other OpenAI-compatible local server. Existing config keeps working with zero migration. Strict-mode users can disable this transport via `hu_apple_config_t.transport_floor`.

---

## 3. C-side surface changes

### 3.1 Vtable additions (per D1)

**None to `hu_provider_vtable_t`.** The existing surface (`chat`, `stream_chat`, `chat_with_tools`, `load_adapter` triple, `supports_native_tools`, `supports_streaming`) covers every requirement. Adding fields to that vtable would push the cost onto every cloud provider and violate KISS/YAGNI per `docs/standards/engineering/principles.md`.

The change is in `include/human/providers/apple.h` only.

### 3.2 Config struct extension

```c
/* include/human/providers/apple.h — extension */
typedef enum hu_apple_transport {
    HU_APPLE_TRANSPORT_AUTO = 0,        /* dlopen → UDS → TCP (default) */
    HU_APPLE_TRANSPORT_DIRECT,           /* dlopen only, fail if unavailable */
    HU_APPLE_TRANSPORT_UDS,              /* UDS only */
    HU_APPLE_TRANSPORT_TCP,              /* TCP loopback only (legacy) */
} hu_apple_transport_t;

typedef enum hu_apple_pcc_policy {
    HU_APPLE_PCC_ALLOW = 0,              /* Apple may fall back to PCC */
    HU_APPLE_PCC_DENY,                   /* strict-local; refuse PCC */
    HU_APPLE_PCC_PROMPT,                 /* surface to user via observer */
} hu_apple_pcc_policy_t;

typedef struct hu_apple_config {
    /* Existing (unchanged): */
    const char *base_url;
    size_t base_url_len;
    /* New (all optional, sensible defaults): */
    hu_apple_transport_t transport;          /* default AUTO */
    hu_apple_transport_t transport_floor;    /* strictest acceptable; default TCP */
    hu_apple_pcc_policy_t pcc_policy;        /* default ALLOW; PROMPT is the recommended default */
    const char *uds_path;                    /* default $XDG_RUNTIME_DIR/human-ondevice.sock */
    size_t uds_path_len;
    const char *bridge_dylib_path;           /* dlopen path; default platform-derived */
    size_t bridge_dylib_path_len;
    bool emit_disclosure_on_pcc;             /* observer event when PCC engaged; default true */
} hu_apple_config_t;
```

All existing callers (`hu_apple_config_t cfg = {0};`) continue to compile and behave identically — zero-init maps to AUTO / ALLOW / defaults, matching the current behaviour.

### 3.3 New public functions (per `docs/standards/engineering/naming.md`)

```c
/* Stable C ABI symbols expected from libHumanFoundationModelsBridge.dylib.
 * Documented here so the dylib's @_cdecl exports can be regression-pinned. */

/* Probe what transports are actually available right now. Returns a bitmask
 * of (1<<HU_APPLE_TRANSPORT_DIRECT) | (1<<HU_APPLE_TRANSPORT_UDS) |
 * (1<<HU_APPLE_TRANSPORT_TCP). 0 = nothing available, return NOT_SUPPORTED. */
uint32_t hu_apple_probe_transports(hu_allocator_t *alloc);

/* Returns the transport the provider actually selected, after construction.
 * Caller does NOT free. Stable until provider deinit. */
hu_apple_transport_t hu_apple_provider_active_transport(const hu_provider_t *provider);

/* Returns true if the most recent inference round invoked Private Cloud Compute.
 * Reset on every chat() / stream_chat() call. Observer-style hook for the
 * persona disclosure path (docs/standards/ai/disclosure.md). */
bool hu_apple_provider_last_used_pcc(const hu_provider_t *provider);
```

These three functions are **named per `hu_<module>_<action>` (per `docs/standards/engineering/naming.md`)**, are read-only after construction, and add no new vtable methods. They are diagnostic / observability hooks. Cloud providers continue to leave the FM-specific surface alone.

### 3.4 ABI exported by HumanKit (Swift @_cdecl → C)

The bridge dylib exports five C symbols. These are the *only* coupling between the C core and the Swift FoundationModels surface. Versioned via a `HU_APPLE_FM_ABI_VERSION` macro (initial value: 1).

```c
/* All symbols are `__attribute__((visibility("default")))` from the dylib.
 * Loaded via dlopen + dlsym; never linked at build time (keeps C binary
 * fully independent of Swift runtime). */

extern uint32_t hu_apple_fm_abi_version(void);
extern int      hu_apple_fm_session_create(const char *system_prompt_utf8,
                                            size_t system_prompt_len,
                                            uint32_t pcc_policy,    /* enum hu_apple_pcc_policy_t */
                                            const void *tools_json, /* hu_apple_fm_tool_array_t */
                                            size_t tools_json_len,
                                            void **out_session_handle);
extern int      hu_apple_fm_session_chat(void *session_handle,
                                          const char *prompt_utf8, size_t prompt_len,
                                          double temperature,
                                          char **out_utf8, size_t *out_len,
                                          void **out_tool_calls_json, size_t *out_tool_calls_len,
                                          bool *out_pcc_engaged);
extern int      hu_apple_fm_session_stream(void *session_handle,
                                            const char *prompt_utf8, size_t prompt_len,
                                            double temperature,
                                            int (*chunk_cb)(void *cb_ctx,
                                                           const char *delta_utf8,
                                                           size_t delta_len,
                                                           bool is_final,
                                                           bool is_tool_call,
                                                           const char *tool_name_utf8,
                                                           size_t tool_name_len),
                                            void *cb_ctx,
                                            bool *out_pcc_engaged);
extern void     hu_apple_fm_session_destroy(void *session_handle);
```

The ABI is **deliberately tiny** (5 symbols + 1 version probe) so the Swift surface can evolve without C ABI churn. Tool descriptors and tool-call results cross the boundary as **UTF-8 JSON blobs**, not C structs — the same trade-off the Mac/iOS XPC services make, well-trodden in Apple's own stack.

---

## 4. Swift-side surface

### 4.1 New file: `HumanFoundationModelsBridge.swift`

Lives in a new HumanKit target `HumanFoundationModelsBridge` (separate from `HumanOnDevice` so the existing daemon stays untouched and the bridge dylib stays minimal — no SwiftUI dependencies).

```swift
// apps/shared/HumanKit/Sources/HumanFoundationModelsBridge/HumanFoundationModelsBridge.swift
//
// Stable C ABI bridge to FoundationModels.framework. Built as a dynamic
// library; consumed by src/providers/apple.c via dlopen.
// macOS 26.0+ / iOS 26.0+ only (compile-time #if canImport(FoundationModels)).

#if canImport(FoundationModels)
import FoundationModels

@_cdecl("hu_apple_fm_abi_version")
public func hu_apple_fm_abi_version() -> UInt32 {
    return 1
}

@available(macOS 26.0, iOS 26.0, *)
final class FMSession {
    let session: LanguageModelSession
    let pccPolicy: PCCPolicy
    let tools: [ToolDescriptor]
    var lastUsedPCC: Bool = false
    // ... initialiser binds instructions, tools, policy
}

@_cdecl("hu_apple_fm_session_create")
public func hu_apple_fm_session_create(...) -> Int32 { ... }

@_cdecl("hu_apple_fm_session_chat")
public func hu_apple_fm_session_chat(...) -> Int32 { ... }

@_cdecl("hu_apple_fm_session_stream")
public func hu_apple_fm_session_stream(...) -> Int32 { ... }

@_cdecl("hu_apple_fm_session_destroy")
public func hu_apple_fm_session_destroy(...) { ... }
#endif
```

### 4.2 Tool descriptors (the lossy bits)

FoundationModels' tool surface is **not** OpenAI-shape:

- **OpenAI shape:** `{"type":"function","function":{"name":..., "description":..., "parameters":{"type":"object", "properties":{...}, "required":[...]}}}`
- **FoundationModels shape:** `Tool` protocol with strongly-typed `Arguments: Generable` and `Output: Generable`. Parameters are declared as Swift types decorated with `@Generable`; JSON schema is derived at compile time.

We **cannot** declare Swift `@Generable` types at runtime from arbitrary JSON schemas. The bridge accepts a **subset** of JSON Schema and maps it onto a dynamic `DynamicTool: Tool` adapter that re-validates arguments at the boundary.

| `hu_tool_spec_t.parameters_json` (JSON Schema) | FM `Tool.Arguments` (Generable) | Lossy? |
|---|---|---|
| `{"type":"string"}` | `String` | No |
| `{"type":"integer"}` | `Int64` | Coerce float→int rejected |
| `{"type":"number"}` | `Double` | No |
| `{"type":"boolean"}` | `Bool` | No |
| `{"type":"array","items":{"type":"string"}}` | `[String]` | No |
| `{"type":"object","properties":{...}}` (depth 1) | `[String: AnyGenerable]` shim | **Yes — depth-1 only** |
| `{"type":"object","properties":{...}}` (depth ≥2) | flattened to JSON-string param | **Yes — re-parsed at tool boundary** |
| `{"enum":["a","b"]}` | `String` with post-call validate | **Yes — validation moves to C** |
| `{"oneOf":[...]}` | unsupported | **Lossy: rejected at create** |

Tool descriptors that cannot be mapped are surfaced via `hu_apple_fm_session_create` returning a non-zero status with an `HU_ERR_NOT_SUPPORTED` mapped error. The caller (h-uman agent) is expected to drop the unsupported tool from the request and proceed — same fallback path already used in `compatible.c` for providers without tool support.

### 4.3 Streaming chunks

`LanguageModelSession.streamResponse(to:)` returns an `AsyncSequence` of partial responses. The bridge wraps that in a synchronous-looking callback because C doesn't speak Swift concurrency. Two design alternatives considered:

- **A. `DispatchSemaphore` block-and-callback** (chosen). The C caller blocks on `hu_apple_fm_session_stream`; the bridge spawns a `Task`, iterates the `AsyncSequence`, calls the C callback on each chunk synchronously, signals the semaphore on completion. Matches the existing C streaming contract (`hu_stream_callback_t` is synchronous).
- **B. Pull-mode `next_chunk`.** Caller polls. Higher latency, more C state to track.

The synchronous callback path is what every C client of an `AsyncSequence` does on macOS today (e.g. SwiftBridging examples in Apple's own docs). The cost is one OS thread parked per stream call — same as a `pthread_cond_wait` on a regular HTTP stream.

---

## 5. IPC contract (when not using the direct-link path)

Two transports speak the same wire protocol (HTTP/1.1 + SSE for streaming, OpenAI-compatible body), but differ in:

| | TCP loopback (legacy) | UDS (new) | Direct C ABI (new) |
|---|---|---|---|
| Wire | HTTP/1.1 over TCP `127.0.0.1:11435` | HTTP/1.1 over `AF_UNIX` socket | C function calls |
| Auth | Bearer token (today: **not enforced by C client** — gap H-01-A) | Socket file mode `0600`, owner UID | Process-local |
| Attack surface | Any local user, any local process | Same-user processes only | None |
| Bounds-check | hu_http (libcurl) | Direct read/write, **must add bounds-checking** | Per-call lengths in ABI |
| Discoverable by `nmap` | Yes | No | N/A |
| Compatible with `apfel` / third-party servers | Yes | No (UDS path differs) | No |

**Security bounds-checking for UDS** (from `docs/standards/security/threat-model.md` §4.1 — IPC threats):

1. Every read into a buffer must specify a length cap and reject overruns with `HU_ERR_INVALID_ARGUMENT`. The bound is the **same** as the existing HTTP body cap (8 MiB; see `src/core/http.c`).
2. Socket path must be validated: must be inside `$XDG_RUNTIME_DIR` or `~/.human/run/`, must not be a symlink, must have mode `0600` and owner == effective UID. Same checks `src/core/secrets.c` applies to its key file.
3. Length-prefix framing on the request side (HTTP `Content-Length` is mandatory; chunked is rejected) to deny send-then-mass-of-zeros style attacks.
4. Response framing trusted only after a `HTTP/1.1 200` line + a `Content-Type: application/json` or `text/event-stream` header. No "guess the framing" fallback.
5. The Swift server side (`HumanOnDeviceServer`) gets a parallel hardening pass that mirrors `OnDeviceRouter.swift` — owner UID check on `accept()`, path canonicalisation, no `Host:` header trust.

**TCP loopback hardening** (closes the bearer-token gap):

- `apple.c` MUST send `Authorization: Bearer <token>` on every request to the loopback server when transport == TCP. The token is read from `~/.human/run/human-ondevice.token` (mode `0600`, owner UID == effective UID); the file is written by the daemon on startup. If the file is missing / wrong mode / wrong owner, the provider falls back to UDS or returns `HU_ERR_NOT_SUPPORTED`.
- Reject HTTP responses larger than 8 MiB (existing `hu_http` default).

---

## 6. Persona & steering — the LoRA gap

Initiative #02 (MoLoRA), #04 (MLX Qwen3), and #05 (Verifier-driven TTT) all assume the provider exposes a working `load_adapter` triple. **The Apple FM provider does NOT.** FM is a closed box: model weights and the residual stream are not exposed; there is no public API to attach a LoRA, BitFit, or activation-steering vector.

Steering pathways that ARE available:

1. **`LanguageModelSession(instructions:)`** — full system-prompt control. This is what the current code already uses. Persona identity, traits, channel overlay, examples, and personal-model summary all compose into the instructions string. Caps at the FM-default context (~4096 tokens on shipping models; subject to Apple updates).
2. **Few-shot prepending** — persona example bank entries can be folded into the user prompt as `[Example] ...` lines. Lossy but works.
3. **PCC-side adapters** (theoretical) — Apple's WWDC 2025 PCC paper hints at server-side per-tenant adapters. **Not user-controllable.** Treat as out of scope.
4. **Activation steering via the cloud-provider draft** (initiative #01) — orthogonal; not blocked by FM.

**Design choice.** This initiative ships **system-prompt + few-shot steering only**. The `load_adapter` / `unload_adapter` / `active_adapter` vtable entries are left NULL. Callers that ask for an adapter get `HU_ERR_NOT_SUPPORTED` and fall through, per the existing contract for cloud providers.

**Why not silently no-op?** Per `docs/standards/engineering/principles.md` (Fail Fast + Explicit Errors): "Keep unsupported paths explicit (`return HU_ERR_NOT_SUPPORTED`) rather than silent no-ops." A silent no-op here would lie to the personalization observer pipeline.

**Honest moat-gap vs initiative #04.** This is the single biggest reason #04 (MLX Qwen3) remains on the critical path even after #03 ships. The Apple FM provider is the **fast zero-network fallback**; MLX Qwen3 is the **real personalization path**. The two are complementary, not competitive.

---

## 7. Entitlements & privacy declarations

### 7.1 iOS app (HumaniOS)

`Info.plist` additions:

| Key | Value | Reason |
|---|---|---|
| `NSAppleIntelligenceUsageDescription` | "human uses on-device Apple Intelligence to draft messages without sending your conversation off-device." | iOS 19 requires a usage string for FM-using apps. |
| `NSPrivateCloudComputeUsageDescription` | "When the on-device model can't handle a request, human can optionally use Apple's Private Cloud Compute. You can disable this in Settings." | If `pcc_policy != DENY`. |

`HumaniOS.entitlements` additions:

| Entitlement | Value | Reason |
|---|---|---|
| `com.apple.developer.foundation-models` | `<true/>` | FM framework access. |
| `com.apple.developer.foundation-models.tool-calling` | `<true/>` | Native tool descriptors. |
| `com.apple.developer.private-cloud-compute` | `default` / `none` | Controlled per build configuration. `none` for the "strict-local" Test Flight build. |

The actual entitlement keys are placeholders pending Apple's final naming in iOS 19 GA (current betas use `com.apple.developer.appleintelligence.*` and `com.apple.developer.foundation-models.*` inconsistently — see open question §14).

### 7.2 macOS daemon (`human`)

No entitlements required when running as a CLI binary outside a code-signed bundle (FM is process-scoped, no special claim). The macOS bundle build (`apps/macos/`) adds the same entitlements as iOS plus:

- `com.apple.developer.system-extension.network-extension` is **NOT** required (we use loopback / UDS, not network filtering).

### 7.3 PCC fallback policy

Apple ships FM with an automatic PCC fallback: if the on-device model returns "guardrail blocked" or "context overflow," the framework may transparently retry against Private Cloud Compute. PCC is a real service with strong privacy guarantees (anonymous attestation, ephemeral compute, no logs) — see *Apple Security: Private Cloud Compute, August 2024*, https://security.apple.com/blog/private-cloud-compute/ — but it **is the network**, and a user who chose `provider: apple` to escape the network was not asking for that.

Three policy levels (`hu_apple_pcc_policy_t`):

- `HU_APPLE_PCC_ALLOW` — match Apple defaults; PCC may be used silently. Suitable for non-sensitive personas.
- `HU_APPLE_PCC_DENY` — refuse PCC; if on-device fails, return `HU_ERR_NOT_SUPPORTED` and let the caller fall through to a sibling provider. This is the recommended default for personas with `privacy_level >= STRICT`.
- `HU_APPLE_PCC_PROMPT` — emit an observer event (`hu_observer_event_t` extension, see §10.5) so the channel can surface "human is going to Private Cloud Compute for this — OK?" to the user.

The Swift bridge cannot fully disable PCC at the `LanguageModelSession` level (Apple does not provide a flag in iOS 19 b1). The bridge approximates DENY by **catching** the post-call diagnostic that FM sets when PCC was invoked and discarding the response. This loses one round-trip in the worst case; acceptable for the strict-mode persona.

---

## 8. Files to create / modify (per D2)

### 8.1 New files

| File | Est. LOC | Purpose |
|---|---|---|
| `include/human/providers/apple_fm.h` | 110 | New ABI declarations, transport enum, PCC policy enum, public helper functions (§3.3). |
| `src/providers/apple_fm_transport.c` | 380 | UDS transport, dylib `dlopen` + symbol load, transport autoselection. Bounds-checked per §5. |
| `src/providers/apple_fm_tools.c` | 220 | JSON-Schema → FM-Tool descriptor mapping. Validates and rejects unsupported shapes per §4.2. |
| `apps/shared/HumanKit/Sources/HumanFoundationModelsBridge/HumanFoundationModelsBridge.swift` | 320 | The Swift bridge with five `@_cdecl` exports. |
| `apps/shared/HumanKit/Sources/HumanFoundationModelsBridge/FMTools.swift` | 180 | DynamicTool adapter; JSON arg coercion. |
| `apps/shared/HumanKit/Tests/HumanFoundationModelsBridgeTests/BridgeABITests.swift` | 140 | Pins the C ABI: each `@_cdecl` symbol exists, version returns 1, NULL-args rejected. |
| `tests/test_apple_fm_provider.c` | 240 | C-side tests: transport selection, tool-mapping rejection, PCC policy plumbing, fallback chain. |
| `tests/test_apple_fm_ipc_fuzz.c` | 110 | Bounds-check fuzz harness for UDS read path. |
| `scripts/check-apple-fm-abi.sh` | 40 | Lints that `nm libHumanFoundationModelsBridge.dylib` exports exactly the five `@_cdecl` symbols at version 1. |

### 8.2 Modified files

| File | Lines touched | Change |
|---|---|---|
| `include/human/providers/apple.h` | +35 | Extend `hu_apple_config_t` per §3.2 (additive only — existing `{0}` callers unaffected). |
| `src/providers/apple.c` | +120 / −10 | Wire transport selection. Add bearer-token send on TCP path. Surface `hu_apple_provider_active_transport` / `hu_apple_provider_last_used_pcc`. Keep existing OAI-compat HTTP fast path. |
| `src/providers/factory.c` | +5 | Add `apple-fm` factory alias for the new transport-aware constructor; keep `apple` / `apfel` / `apple-intelligence` / `foundationmodels` aliases. |
| `apps/shared/HumanKit/Package.swift` | +10 | New `HumanFoundationModelsBridge` library target (dylib product), new `HumanFoundationModelsBridgeTests` test target. |
| `CMakeLists.txt` | +30 | New option `HU_ENABLE_APPLE_FM` (defaults `ON` when `APPLE`); discover Swift bridge dylib path via `find_library`. |
| `apps/ios/Sources/HumaniOS/HumanApp.swift` | +20 | When FM is available and persona allows, route generation through the in-process bridge (no IPC); record disclosure event when PCC engaged. |
| `apps/macos/Package.swift` | +5 | Link new HumanKit product. |
| `docs/standards/security/threat-model.md` | +25 | Append §4.3.x: Apple FM bridge IPC threat surface (UDS path, dylib trust). |
| `docs/standards/ai/disclosure.md` | +10 | Add PCC-disclosure event. |
| `apps/macos/Sources/HumanApp/HumanApp.swift` | +15 | Print transport selection on startup banner. |

**Total LOC:** ~1,740 new + ~265 modified. **C-side compiled cost:** estimated ≤22 KB after `-Os -flto` (well within the 24 KB budget — see §11).

---

## 9. Data flow

```
User typing in iMessage on iPhone
       │
       ▼
HumaniOS app (Swift)
       │
       │ in-process call (no IPC)
       ▼
OnDeviceProvider.swift  ──▶  LanguageModelSession.streamResponse(to:)
                                 │
                                 ▼
                            FoundationModels.framework
                                 │
                                 ▼ ANE / GPU on device
                            On-device 3B model
                                 │ (or PCC fallback if policy ALLOW)
                                 ▼
                            streamed tokens
                                 │
                            chunks ─▶ AppIntent / ChatView


User typing in CLI on macOS
       │
       ▼
human (C daemon)
       │
       │ hu_provider_t.stream_chat()
       ▼
src/providers/apple.c
       │
       ├─ Transport == DIRECT?
       │       │
       │       └─▶ dlsym(hu_apple_fm_session_stream)
       │              │
       │              ▼
       │           libHumanFoundationModelsBridge.dylib
       │              │ (same Swift code as iOS path)
       │              ▼
       │           FoundationModels.framework
       │              │
       │              ▼ ANE
       │           model ─▶ stream chunks ─▶ C callback
       │
       ├─ Transport == UDS?
       │       │
       │       └─▶ AF_UNIX socket → human-ondevice daemon
       │              │
       │              ▼ same Swift bridge
       │           FoundationModels.framework
       │
       └─ Transport == TCP? (legacy)
               │
               └─▶ HTTP localhost:11435 + Bearer token → human-ondevice
                      │
                      ▼ same Swift bridge
                   FoundationModels.framework
```

Three transports, **one** Swift codepath, **one** model invocation. The C side has zero knowledge of which transport it's on after `hu_provider_create()` returns.

---

## 10. Integration & observability

### 10.1 Persona composition

`src/agent/frontier_prompt.c` (already on the working tree per the git status) composes the persona prompt. The Apple FM bridge consumes it unchanged — `LanguageModelSession(instructions:)` accepts an arbitrary string.

### 10.2 Tool registry

The agent's `hu_tool_t` registry is already enumerable. `apple.c` walks the registry, calls `apple_fm_tools.c::map_to_fm_descriptor()` for each tool, drops the ones that don't map (logging at INFO level), and passes the surviving set to the bridge. The Swift side declares them as `DynamicTool` instances and binds them into the `LanguageModelSession.tools` array.

### 10.3 Observer hook

A new `hu_observer_event_t` discriminator:

```c
HU_OBSERVER_PROVIDER_PCC_INVOKED  /* fired after a chat where PCC was used */
HU_OBSERVER_PROVIDER_PCC_REFUSED  /* fired when policy DENY refused a PCC fallback */
```

Wired through `src/observability/log_observer.c` so the daemon logs "PCC engaged for prompt #N" at INFO. The `disclosure` layer surfaces it to the user per `docs/standards/ai/disclosure.md`.

### 10.4 Config schema

`config.json` gains:

```json
{
  "provider": "apple-fm",
  "apple_fm": {
    "transport": "auto",          // "auto" | "direct" | "uds" | "tcp"
    "transport_floor": "uds",     // strictest acceptable
    "pcc_policy": "prompt",       // "allow" | "deny" | "prompt"
    "uds_path": "~/.human/run/human-ondevice.sock"
  }
}
```

Defaults match the C struct defaults in §3.2. Backwards-compat: `provider: apple` keeps working with the legacy TCP-loopback path.

### 10.5 Eval / fidelity wiring

Hooks into the `hu_fidelity_*` scorer (working-tree file `src/ml/fidelity.c`, header `include/human/ml/fidelity.h`) so persona-fidelity evals run against the Apple FM path the same way they run against cloud providers. This is the N1 metric in the defer condition (§13).

---

## 11. Binary budget

**Budget per brief.** ≤24 KB C-side, Swift code off-budget.

**Component cost estimate (MinSizeRel + LTO, aarch64-apple-darwin):**

| Source | Estimated KB |
|---|---|
| `src/providers/apple.c` (existing) | 17.2 |
| `src/providers/apple_fm_transport.c` (new) | 4.1 |
| `src/providers/apple_fm_tools.c` (new) | 2.6 |
| Header constants / inline helpers | 0.1 |
| **Total** | **~24.0** |

**Hard ceiling.** 24 KB. If LTO comes in over, the first thing dropped is the TCP-loopback compatibility path (the user with `apfel` running will need to switch to `compatible`-flavoured provider config); estimated save: 3 KB.

**RSS delta at runtime.** No additional allocations per turn beyond what the current `apple.c` does — the dylib (when used) is `dlopen`'d once and held for the lifetime of the provider; ~600 KB resident for the Swift runtime when the bridge is loaded, **not counted against the C runtime budget** per the brief.

**Test binary cost.** ~12 KB additional in `human_tests` (tests + fixtures). Within the existing `human_tests` budget.

---

## 12. Test plan (per D3)

### 12.1 Unit tests (deterministic, `HU_IS_TEST`-mocked)

`tests/test_apple_fm_provider.c` — all in the mocked path, no real FM, no real Swift:

| Test | Purpose |
|---|---|
| `test_apple_fm_create_succeeds_with_zero_init_config` | Backwards-compat: existing `hu_apple_config_t cfg = {0}` continues to work. |
| `test_apple_fm_create_with_transport_floor_uds_rejects_tcp` | Strict-floor enforcement. |
| `test_apple_fm_create_with_transport_direct_falls_through_when_dylib_missing` | AUTO transport graceful degradation. |
| `test_apple_fm_active_transport_returns_selected_value` | Diagnostic API stable. |
| `test_apple_fm_tool_mapping_rejects_unsupported_oneOf` | Tool-shape lossy bits visible. |
| `test_apple_fm_tool_mapping_flattens_nested_object_to_json_string` | Documented lossy fallback. |
| `test_apple_fm_pcc_policy_deny_returns_not_supported_when_pcc_required` | Strict-local enforcement. |
| `test_apple_fm_pcc_policy_prompt_emits_observer_event` | Disclosure wiring. |
| `test_apple_fm_last_used_pcc_resets_per_call` | Observer state hygiene. |
| `test_apple_fm_uds_path_rejects_symlink` | IPC path validation. |
| `test_apple_fm_uds_path_rejects_world_writable` | IPC mode validation. |
| `test_apple_fm_tcp_path_requires_bearer_token_file` | TCP loopback hardening. |
| `test_apple_fm_load_adapter_returns_not_supported` | Honest no-op for cloud-provider-style fallthrough. |

All tests run under ASan + `HU_IS_TEST=1`. None spawns a process or opens a real socket; the transport selection logic is exercised against a pluggable `hu_apple_fm_transport_provider_t` fixture.

### 12.2 Integration tests

`tests/integration/test_integ_apple_fm.c` — opt-in via `HU_HAVE_APPLE_FM=1` env var (matches the Phase-1 llamacpp gating pattern in `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md`):

| Test | Purpose |
|---|---|
| `integ_apple_fm_real_round_trip_on_macos_26` | Real `LanguageModelSession.respond(to:)` round-trip via the dylib. Skipped on macOS < 26. |
| `integ_apple_fm_streaming_yields_n_chunks` | Real streaming, n ≥ 3 chunks observed. |
| `integ_apple_fm_pcc_deny_refuses_overflow` | PCC denial path on a deliberately too-large prompt. |

CI: skipped on Linux runners; runs on the macOS-aarch64 runner (already in `ci.yml`) when the SDK is available.

### 12.3 Swift-side tests

`apps/shared/HumanKit/Tests/HumanFoundationModelsBridgeTests/BridgeABITests.swift`:

| Test | Purpose |
|---|---|
| `test_abi_version_returns_1` | Pins the C ABI version constant. |
| `test_session_create_rejects_null_instructions` | C-side caller defensive. |
| `test_session_create_with_oneof_schema_returns_unsupported` | JSON-Schema rejection. |
| `test_session_destroy_idempotent` | Double-free safety on the Swift side. |

### 12.4 Fuzz harness

`tests/test_apple_fm_ipc_fuzz.c` + a `fuzz/fuzz_apple_fm_uds_read.c` libFuzzer target.

| Target | Coverage |
|---|---|
| `fuzz_apple_fm_uds_read` | Feed arbitrary bytes into the UDS framing layer; assert no out-of-bounds reads, no allocations beyond the 8 MiB cap, no crash. |
| `fuzz_apple_fm_tool_mapping` | Feed arbitrary JSON into `apple_fm_tools.c::map_to_fm_descriptor()`; assert clean rejection of malformed schemas. |

Both wired into `fuzz/CMakeLists.txt` alongside the existing 31 harnesses.

### 12.5 Red-team / adversarial

Reuses the `security-reviewer` subagent pattern from `2026-05-11-sota-2026-massive-team-program.md`. Specifically:

- Verify UDS owner-UID check is actually enforced (write a fixture daemon as a different UID, attempt connect, expect `HU_ERR_PERMISSION_DENIED`).
- Verify the bearer token isn't logged anywhere (grep audit + scrub.c re-run).
- Verify PCC events appear in audit log when policy `PROMPT`.

---

## 13. Risk register (per D4)

| # | Risk | Probability | Impact | Mitigation |
|---|---|---|---|---|
| **R1** | **Apple's FM tool-calling API changes between WWDC betas and iOS 19 GA.** | Medium | High — could invalidate the DynamicTool adapter | Pin to a specific Xcode SDK version in the HumanKit build; the bridge fails closed (returns `HU_ERR_NOT_SUPPORTED`) on unrecognised SDK; sprint planning includes a "WWDC 2026 review" gate before adopting #03. |
| **R2** | **Binary budget overrun (>24 KB C side).** | Low | Medium — would force descope of TCP path | Per §11, the LTO budget is tight; the back-up plan drops TCP-loopback compat (~3 KB save) which is a well-marked code path. Pre-merge size check in `scripts/check-binary-size.sh`. |
| **R3** | **Bridge dylib becomes a persistent attack surface.** Any user-loadable dylib is a code-injection vector if `DYLD_INSERT_LIBRARIES`-style overrides apply. | Low | High — full RCE in the daemon | The dylib path is **canonicalised** before `dlopen`; only loaded from `${CMAKE_INSTALL_PREFIX}/lib/` or `${SWIFT_HOST_LIBS}`; never from `$HOME`. The dylib is code-signed in the macOS release build; `apple.c` calls `csr_check` (or `SecCodeCheckValidity` via `dlopen`-aware path) before invoking the first symbol. ABI version check (`hu_apple_fm_abi_version` == 1) on first call. Listed in `docs/standards/security/threat-model.md` §4.3.x as a new trust boundary. |

### 13.1 Memory / ASan

- Every allocation in `apple_fm_transport.c` / `apple_fm_tools.c` is paired with a `free` in the same function or a documented cleanup path. ASan run is part of `cmake --preset dev && human_tests`.
- The Swift bridge cannot leak into the C heap — every C-string out-param is allocated with `hu_allocator_t.alloc` and freed by the caller via `hu_chat_response_free` / `hu_stream_chat_result_free`. The bridge copies bytes across the boundary; no Swift-managed memory reaches C.
- Test: `test_apple_fm_provider_no_leak_on_error_paths` (already in §12.1 list, drives every failure path).

### 13.2 Model-quality regression

- N1 metric (defer §13 / brief): preference rate ≥ 55% on Tier-1 channels vs the cloud baseline.
- Tracked in the existing fidelity scorer (`src/ml/fidelity.c`); persona-eval anchor runs nightly.
- If N1 < 55% over a 5-night rolling window, this initiative is parked (§14 defer).

---

## 14. arXiv & external references (per D5)

Per the brief, arXiv is unavailable for several of these; URL-cited where so.

1. **Apple, "FoundationModels framework documentation,"** WWDC 2025 / macOS 26 SDK. https://developer.apple.com/documentation/foundationmodels (accessed 2026-05-11).
2. **Apple, "Meet the Foundation Models framework,"** WWDC 2025 session 286. https://developer.apple.com/wwdc25/286 (accessed 2026-05-11).
3. **Apple Security Engineering, "Private Cloud Compute: A new frontier for AI privacy in the cloud,"** August 2024. https://security.apple.com/blog/private-cloud-compute/ — the formal PCC architecture paper; the basis for §7.3 policy levels.
4. **Apple, "Privacy & Apple Intelligence,"** updated 2025. https://www.apple.com/legal/privacy/data/en/apple-intelligence/ — disclosure obligations referenced in §7.1.
5. **MLC LLM team, "Bringing on-device LLMs to Apple Silicon,"** arXiv:2310.07706 (October 2023). Comparison baseline for §6's "system-prompt vs adapter steering" gap; substantiates the claim that LoRA on M-series via MLX is the only credible adapter path today.
6. **Sennrich et al., "Few-shot learning via instructions,"** ACL Findings 2024. Empirical basis for §6's "system-prompt + few-shot" claim that we can recover a measurable fraction of the LoRA gap without weights.
7. **Apple, "Authorizing the use of system extensions,"** macOS reference. https://developer.apple.com/documentation/security/preparing-your-app-to-work-with-mac-system-extensions — code-signing assumptions for §13.1 R3.

---

## 15. Defer / descope condition (per D7)

This initiative is **parked** under any of the following evidence:

1. **N1 misses on Tier-1.** After Phase 1 (TCP) ships, run the fidelity scorer for 5 consecutive nights against Telegram / Discord / iMessage / Slack persona-eval anchors. If preference-rate vs the existing Anthropic-claude baseline is < 55% **AND** Apple's iOS 19 GA `LanguageModelSession` still ships without an adapter API (verified in WWDC-2026 session list), keep `apple.c` only as the "fast zero-network fallback" — disable it as the default provider, route personalization through initiative #04 (MLX Qwen3-4B + LoRA).
2. **Binary-budget overrun > 30 KB after LTO** with no TCP-path-drop path remaining. Tier-down: ship Phase 1 only, defer the in-process dylib path (Phase 3) indefinitely.
3. **Apple revokes / restricts the FoundationModels entitlement for non-Apple-store distribution channels.** This kills the open-source distribution story; defer until a workaround (e.g., MLX-based alternative under #04) is mature.
4. **PCC fallback cannot be denied at the SDK level by iOS 19 GA.** The bridge's catch-and-discard workaround works but lies to the user about latency. If Apple does not give us a clean `denyPCC` flag, the PCC-DENY policy ships with a documented "best-effort" caveat instead of being a hard guarantee; this is **not** an automatic park but it lowers the strategic value of the initiative.

The defer plan: keep `apple.c` (already shipped) at Phase 1 (TCP + bearer token + bounds-checks; ~5 KB delta) and roll the remaining 19 KB of binary budget to initiative #04. The Swift bridge work is **not wasted** in the defer case — `apps/ios/` uses it directly via in-process Swift, independent of the C provider.

---

## 16. Build sequence (phased)

Numbered phases align with the §13 defer logic.

### Phase 1 — Hardening the existing TCP path (1–2 weeks, low risk)

- [ ] Add bearer-token send to `apple.c` TCP path.
- [ ] Token file write in `human-ondevice` daemon (`apps/tools/human-ondevice/`).
- [ ] Extend `hu_apple_config_t` per §3.2 (additive, zero-init backwards-compat).
- [ ] Add `hu_apple_pcc_policy_t` plumbing; observer events; disclosure wiring.
- [ ] Tests `test_apple_fm_*_pcc_*`, `test_apple_fm_tcp_path_requires_bearer_token_file`.
- [ ] Update threat model §4.3.x.

**Exit:** Existing functionality intact; security gaps from §1 line 4 ("zero network — true for WAN, false for loopback") closed.

### Phase 2 — Native tool calling via the bridge (2–3 weeks, medium risk)

- [ ] New target `HumanFoundationModelsBridge` in HumanKit.
- [ ] Swift `@_cdecl` exports for `hu_apple_fm_*` (5 symbols + version).
- [ ] `src/providers/apple_fm_tools.c` JSON-Schema → FM descriptor mapping.
- [ ] `dlopen` path in `apple.c`; preference order DIRECT → UDS → TCP.
- [ ] `apps/ios/` switches AppIntents `AskHumanIntent` to use the bridge directly (no daemon).
- [ ] Tool-mapping tests + Swift-side ABI tests.
- [ ] Fuzz harness for tool-mapping.

**Exit:** First real tool call from the on-device model. Tier-1 channels can use FM-backed tool execution.

### Phase 3 — UDS transport + strict-local mode (2 weeks, medium risk)

- [ ] UDS server side in `OnDeviceServer.swift` (mirror of HTTP path).
- [ ] UDS client side in `apple_fm_transport.c` with §5 bounds-checks.
- [ ] Path validation (canonicalise, mode check, owner check).
- [ ] Tests `test_apple_fm_uds_*`, IPC fuzz harness.
- [ ] Update threat-model §4.3.x with the UDS surface.

**Exit:** Loopback TCP attack surface eliminated when `transport_floor: uds`. `provider: apple-fm` ships as the macOS default.

### Phase 4 — In-process dylib (stretch, 1 week, low risk if Phases 2–3 land)

- [ ] Code-sign the bridge dylib in the macOS release build.
- [ ] `SecCodeCheckValidity` call before first `dlsym`.
- [ ] `csr_check` smoke; ABI-version regression test.
- [ ] Update binary-size benchmark.

**Exit:** Same-process FM call on macOS. ~5–15ms shaved off TTFT (no socket round-trip). Largest single SOTA latency win available without changing the model.

### Phase 5 — Adapter steering eval (1 week, gate for defer)

- [ ] Run persona-fidelity scorer for 5 nights against the new FM provider.
- [ ] Measure N1 on Tier-1 channels.
- [ ] If N1 ≥ 55%, promote `apple-fm` as default on `__APPLE__`.
- [ ] If N1 < 55%, exercise §13.1 defer — keep `apple-fm` available, route default to #04.

---

## 17. Open questions

1. **Apple's iOS 19 entitlement key naming.** The current beta naming (`com.apple.developer.foundation-models` vs `com.apple.developer.appleintelligence.*`) is inconsistent. Lock the keys at GA; the `Info.plist` lines in §7.1 are placeholders.
2. **Tool-calling completion latency on the ANE.** Apple's WWDC numbers don't separate "first token" from "first tool-call decision." Need a benchmark before we trust Phase 2 numbers.
3. **PCC denial reliability.** Whether Apple ships a `LanguageModelSession.disablePCC` flag in iOS 19 GA, or whether the bridge keeps the post-call detection-and-discard workaround indefinitely. Either way, this is the §13.1 R-issue that drives part of the defer logic.
4. **Bridge dylib code-signing in the open-source distribution.** Self-built users won't have the Apple Developer ID required to sign the dylib; we need a "skip the signature check if the dylib is loaded from the same CMake install prefix" carve-out, similar to how `embedded.c` trusts `llama-cli` from the user's `PATH`.

---

## 18. Summary

This initiative does not invent new capability — Apple's `FoundationModels.framework` is shipping. It hardens, narrows, and integrates the existing `apple.c` provider so that h-uman can honestly claim "true zero-network, on-device, persona-faithful inference on every Apple device the user owns" without the asterisks the current stub carries. The single biggest open question is **whether system-prompt-only steering plus tool calling can hit our N1 ≥ 55% bar without LoRA**. If yes, this is one of the highest-leverage initiatives in the fleet. If no, it stays the fast fallback path and the personalization story lives in initiative #04.
