---
title: iMessage Abandoned Typing Pattern Feasibility Investigation
date: 2026-03-10
---

# iMessage Abandoned Typing Pattern Feasibility Investigation

**Date:** 2026-03-10
**Feature:** F43 — Abandoned typing pattern
**Status:** Not Feasible (via public APIs)

## Summary

Simulating "started typing then stopped" is **not feasible** using AppleScript, JXA, or any public API. The typing indicator is a real-time signal tied to physical input in Messages.app. The send-then-unsend workaround is also unavailable via public APIs. IMCore private framework exposes both typing and unsend, but carries unacceptable risk for production use.

## Research Findings

### Typing Indicator API

**AppleScript / JXA:** No support. The Messages app script dictionary exposes no properties or commands for triggering or detecting typing indicators. Users have confirmed this via direct inspection of the scripting suite. JXA can send messages via `send("text", {to: buddy})` but has no typing-related verbs.

**Behavior:** The iMessage typing indicator ("..." bubble) is sent by Messages.app when the user physically types. It is a real-time signal from the client to the recipient — not something that can be triggered programmatically through public automation.

**Conclusion:** Typing indicator cannot be triggered via AppleScript or JXA.

### Send + Unsend Approach

**AppleScript / JXA:** No unsend verb. AppleScript can send messages and target specific services (iMessage vs SMS), but there is no equivalent command for retracting or unsending messages.

**Native UI:** macOS Ventura 13+ and iOS 16+ support "Undo Send" via the Messages app UI (right-click → Undo Send). Messages can be unsent within 2 minutes. Both sender and recipient must use compatible OS versions. This is a manual UI action only — not exposed to automation.

**Conclusion:** Send-then-unsend is not possible via AppleScript or JXA. Unsend capability exists in the native app but has no programmatic API. See F44 investigation for unsend feasibility.

### Private Framework (IMCore)

**IMCore** (`/System/Library/PrivateFrameworks/IMCore.framework`) is a private framework that powers Messages.app. Third-party projects (e.g., BlueBubbles) have reverse-engineered it.

**Typing indicator:**

```objc
[chat setLocalUserIsTyping:YES];
[chat setLocalUserIsTyping:NO];
```

`IMChat` objects support `setLocalUserIsTyping:` to start and stop the typing indicator. This would enable the abandoned-typing pattern directly.

**Unsend:**

```objc
[chat retractMessagePart:(item)];
```

`retractMessagePart:` unsends a message part. Combined with send, this would enable the send-then-unsend workaround.

**Risks:**

- Private API — not documented, subject to change or removal in any macOS update
- Incompatible with App Store distribution
- Requires linking against IMCore and running as a native macOS process (not from a pure C/AppleScript bridge)
- Entitlements and sandbox restrictions may block access
- Typing indicators have known timing issues (delays, stuck indicators) even in projects that use IMCore
- Method swizzling may be needed for reliable typing state; adds fragility

**Conclusion:** IMCore technically enables both typing and unsend, but using it is high-risk and would require a separate native macOS helper (e.g., a BlueBubbles-style bundle), not the current AppleScript-based channel.

### Alternative Approaches

1. **Packet-level monitoring:** Theoretically possible to observe typing/unsend at the network layer, but not to _trigger_ them. Not useful for simulating abandoned typing.

2. **Accessibility / UI automation:** Could potentially simulate typing in the Messages input field and then clearing it. Extremely fragile (UI changes, focus, timing), requires full accessibility permissions, and may not produce a clean typing-indicator-then-stop signal. Not recommended.

3. **Send placeholder then edit:** iMessage supports editing (iOS 16+ / macOS Ventura+). Could send a placeholder and edit to final content. This does not replicate "started typing then stopped" — it shows a sent message that was edited, which is a different psychological signal.

4. **Delay + send:** Sending after a delay does not show a typing indicator. The recipient sees nothing until the message arrives.

**Conclusion:** No reliable creative workaround exists. The abandoned-typing pattern requires either direct typing-indicator control (IMCore only) or send-then-unsend (also IMCore-only via public automation).

## Recommendation

**Do not implement** the abandoned typing pattern with the current architecture.

- **Public APIs (AppleScript/JXA):** Not feasible. No typing trigger, no unsend.
- **IMCore:** Technically feasible but high risk. Would require a new native macOS component, private API usage, and ongoing maintenance. Not aligned with KISS/YAGNI or the project's security posture.

**Mark F43 as aspirational** — revisit only if Apple introduces a public typing-indicator or unsend automation API, or if the project adopts a BlueBubbles-style native helper with explicit acceptance of IMCore risk.

**Cross-reference:** F44 (Unsend) investigation will document unsend feasibility in more detail. If F44 concludes unsend is feasible via IMCore, the send-then-unsend path for F43 would share that dependency and risk profile.
