---
status: obsolete
last_audit: 2026-05-17
---

# 2026-05-10 — iMessage Private-API Feasibility (Tier B)

> Design doc only. Nothing in this document is implemented. The "Decision"
> section at the bottom records why we are NOT shipping any of this in the
> 2026-05-10 work cycle, and what conditions must change to revisit.

## Goal

Decide whether to ship send-side native iMessage capabilities that the public
toolchain (`osascript` and the `imsg` CLI) cannot do:

1. **Message effects on send** — Slam, Loud, Gentle, Invisible Ink, Confetti,
   Balloons, etc. (the `expressive_send_style_id` we already *read* on inbound).
2. **Inline / threaded replies** — composing into a specific
   `thread_originator_guid` rather than the latest message in the chat.
3. **Sticker / Memoji send** — `balloon_bundle_id` payloads we read but cannot
   originate.
4. **Edit / unsend** — modifying a message we already sent (`was_edited`,
   `was_unsent` we currently surface on inbound only).

We currently *receive* all of (1)–(4) via `chat.db` polling and surface them
correctly in the agent's context. We can **send** only via AppleScript or `imsg
send`, both of which produce plain text bubbles.

## Why this is hard, not just slow

Sending these requires one of three integration paths, all of which have
significant caveats on macOS 14+ (Sonoma / Sequoia):

### A. IMCore (private framework)

`/System/Library/PrivateFrameworks/IMCore.framework` exposes Objective-C
classes (`IMChat`, `IMMessage`, `IMHandle`, etc.) that the native
**Messages.app** uses internally. We can `dlopen` it and look up classes via
`objc_getClass` — this is not blocked by hardened-runtime policies the way
syscalls are. There is partial precedent in our own tree:

- `src/channels/imessage.c` already declares `imcore_handle`, `imcore_init`,
  `imcore_start_typing`, `imcore_stop_typing` as the typing-indicator path.
- That code currently best-efforts and falls back to AX automation when IMCore
  is unavailable, which is the right pattern.

Real risks:

| Risk | Severity | Notes |
|---|---|---|
| **Selector / class-name churn between macOS versions.** Apple has changed `IMChat` selectors at least twice since macOS 12 (e.g. `sendMessage:` → `sendMessage:adam:`). | High | Each macOS minor release can silently break. Requires a runtime probe with `class_respondsToSelector:` and graceful fallback. |
| **No public ABI guarantee.** Apple can move any of this in a point release. | High | Mitigation: every IMCore call must be wrapped in `class_getInstanceMethod`/`method_invoke` and have an AppleScript fallback. |
| **Sandbox / hardened runtime entitlements.** Production-signed binaries may need `com.apple.security.temporary-exception.shared-preference` or `com.apple.security.cs.allow-dyld-environment-variables` to load PrivateFrameworks at all. | Medium | Currently we are unsigned ad-hoc, which paradoxically makes this *easier* — but is also why TCC keeps revoking FDA. |
| **Threading / RunLoop dependencies.** IMCore uses `NSRunLoop` and main-thread Mac UI conventions. A daemon without an active run loop may queue but never deliver. | High | Our `service-loop` is a plain epoll loop; we'd need either a dispatch queue running a CFRunLoop or to embed the call in a short-lived NSApplication. |
| **App Store / signing implications.** If we ever Notarize, IMCore usage is grounds for rejection. | Low (we don't notarize today) | Future-blocker if we ever ship via App Store / TestFlight for users who want a signed install. |

### B. AccessibilityEvent / `AXUIElement` automation

Drive Messages.app's UI via the Accessibility API to e.g. right-click → "Send
with effect" → "Slam". We already have `ax_open_conversation`,
`ax_start_typing`, `ax_stop_typing`, `ax_tapback`. The pattern is proven.

| Risk | Severity | Notes |
|---|---|---|
| **Brittle UI matching.** Localized macOS strings, light/dark mode, Stage Manager, and reduced-motion all change the AX tree. | High | Requires per-effect XPath-style queries and goldens. |
| **Visible UI flicker.** Effects pickers animate. The user *sees* the cursor move. | Medium | Acceptable for occasional use, awful for high-volume. |
| **Latency.** Each AX action is 50–150 ms; an effect send is 5–8 actions ≈ 800 ms. | Medium | Slower than every other channel we have. |
| **Defeats by Messages.app updates.** Apple ships UI rewrites every 2–3 minor versions. | High | Selector goldens go stale silently. |
| **Cannot do edit / unsend.** Those flows are gated behind a 15-minute window with a confirm dialog that AX cannot script consistently across versions. | High | Edit/unsend should be considered out of scope for the AX path. |

### C. `imagent` XPC service injection

The `imagent` daemon owns the actual iMessage send pipeline; it accepts XPC
messages from `MessagesViewService.xpc` and `imagent`-internal helpers. In
principle we can construct and post compatible XPC messages.

| Risk | Severity | Notes |
|---|---|---|
| **Apple-private XPC interface contracts.** No headers, must be reverse-engineered per macOS version. | Critical | This is the deepest path; effectively a full-time research project to maintain. |
| **System Integrity Protection (SIP).** Some XPC services restrict their port to client binaries with specific entitlements. | High | We almost certainly cannot post directly without SIP off. |
| **Detection and mitigation by Apple.** This is the path that Apple actively monitors and breaks; XPC interface changes are common. | Critical | Worst maintenance cost of any option. |

**This option is not feasible for production. Listed only for completeness.**

## What we'd ship if we did

### Public API additions

```c
/* In include/human/channels/imessage.h, all behind HU_HAS_IMESSAGE.
 * Each takes the channel context, the standard target+text args used by send,
 * plus an effect/style identifier. Returns HU_OK / HU_ERR_* like send. */

hu_error_t hu_imessage_send_with_effect(void *ctx, const char *target, size_t target_len,
                                        const char *text, size_t text_len,
                                        const char *effect_id /* "Slam", "Confetti", etc. */);

hu_error_t hu_imessage_send_inline_reply(void *ctx, const char *target, size_t target_len,
                                         const char *text, size_t text_len,
                                         const char *thread_originator_guid);

/* Sticker/Memoji send and edit/unsend would be deliberately deferred to
 * Phase 2 of this work; their failure modes are user-visible enough that we
 * want extensive shadow-mode telemetry before exposing them. */
```

### Effect-id allowlist

`hu_imessage_effect_name` (already exists) maps `expressive_send_style_id`
strings to display names. We'd reuse that table as the canonical allowlist for
the send side, so the agent cannot ask for a non-existent effect.

### Capability-probe gate

A new helper would runtime-probe IMCore on first use and cache the result on
`hu_imessage_ctx_t`:

```c
typedef enum {
    HU_IMSG_EFFECT_PATH_NONE,        /* not supported on this binary/host */
    HU_IMSG_EFFECT_PATH_IMCORE,      /* preferred */
    HU_IMSG_EFFECT_PATH_AX,          /* fallback */
} hu_imsg_effect_path_t;

hu_imsg_effect_path_t hu_imessage_effect_path(hu_channel_t *ch);
```

Doctor would surface this:

```
ok    [doctor] iMessage effects: IMCore available (Slam, Loud, Gentle, Confetti, ...)
warn  [doctor] iMessage effects: AX fallback only (visible UI flicker on send)
warn  [doctor] iMessage effects: not available — falls back to plain text
```

### Telemetry / shadow mode

Even the IMCore path needs a "rehearse but don't send" mode for the first
weeks: the daemon would compose the IMCore call, log the selector chain it
*would* invoke, and then send via the public path anyway. This catches
selector-name churn before it produces user-visible failures.

## Cost / risk vs. value

| Capability | User-visible value | Build cost (eng-weeks) | Maintenance risk | Failure mode if it breaks |
|---|---|---|---|---|
| Effects on send | Medium — fits "actually feels like a person" | 1–2 (IMCore probe + 1 effect end-to-end) + 1–2 (full effect set) | High (selector churn) | Plain bubble instead of Slam — soft failure |
| Inline reply | High — currently we always reply at the end of the thread, which loses context in busy group chats | 1 (IMCore) or 2 (AX) | High | Reply lands at end of thread instead of inline — soft failure |
| Sticker / Memoji send | Low — sweet but rare in real conversation | 2 (need to construct `balloon_bundle_id` payloads) | Critical (Apple changes balloon schema yearly) | No send / wrong sticker — visible failure |
| Edit / unsend | Low — recoverable, but tempting trap (let the agent "fix" things instead of being right the first time) | 2–3 | High | Wrong message edited / unsent — **hard failure** |

## Decision (2026-05-10)

**Do not implement Tier B in this session.** Reasons:

1. **Tier A is load-bearing and just landed.** The FDA-aware circuit breaker
   and the doctor command depend on the channel staying simple and observable.
   Adding IMCore probes would multiply the surface that can fail in the same
   pathological way. We need at least a week of telemetry from the breaker
   before adding more private-API surface.
2. **Edit / unsend is a trap.** It tempts the agent to "fix" mistakes instead
   of being right the first time, which contradicts the persona thesis (the
   assistant should *be* the person, not perform after-the-fact corrections).
   We should explicitly *not* ship this even if everything else is built.
3. **Inline reply is the only one with clear product value above its risk
   level.** When we revisit Tier B, start there: ship `hu_imessage_send_inline_reply`
   via IMCore with an AX fallback, behind a `HU_IMSG_INLINE_REPLY=ON` build
   flag, and gate it on a `human doctor imessage` "effects path" line.
4. **Effects on send is mainly aesthetic.** Useful for the "feels real" axis
   but not load-bearing; it can wait until inline-reply has shaken out the
   IMCore path on real macOS versions.

## Conditions to revisit

We will reopen Tier B when ALL of these are true:

- [ ] Tier A breaker has been live for 30+ days with zero spurious trips on
      this developer machine.
- [ ] We have at least one user (other than the maintainer) running the daemon
      against their own iMessage long enough to surface real-world corner cases.
- [ ] We have decided whether the binary will ever be Notarized; if yes,
      Tier B is permanently off the table for the App Store build and only
      enabled in the developer build.
- [ ] We have a per-macOS-version selector probe baseline so the IMCore path
      can fail closed (drop to AX, then to plain text) without user-visible
      regressions.

## References

- Existing partial IMCore wiring in `src/channels/imessage.c` (`imcore_init`,
  `imcore_start_typing`, `imcore_stop_typing`).
- Existing AX wiring (`ax_open_conversation`, `ax_start_typing`, `ax_tapback`).
- Existing inbound effect / balloon mapping (`hu_imessage_effect_name`,
  `hu_imessage_balloon_label`).
- `expressive_send_style_id` and `balloon_bundle_id` columns in the poll SQL
  (column 12 / 11) — these are the schema we'd be inverting on send.
