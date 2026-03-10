---
title: iMessage Unsend Feasibility Investigation
date: 2026-03-08
---

# iMessage Unsend Feasibility Investigation

**Date:** 2026-03-08  
**Feature:** F44 — Unsend  
**Status:** Partially Feasible

## Summary

Unsend is **not** exposed via AppleScript or JXA. The only viable programmatic approach is **accessibility/UI automation** (System Events) to right-click the last sent message and choose "Undo Send" — mirroring the existing tapback implementation. This is fragile, requires Accessibility permissions, and is constrained by the 2-minute window. Direct database modification does not trigger the iMessage unsend protocol.

## Research Findings

### AppleScript Messages Dictionary

The Messages app scripting dictionary does **not** expose any "unsend", "delete", "remove", or "retract" verb for individual sent messages.

- **Available commands**: `send` (message text to buddy), login/logout of services, and conversation-level operations.
- **Conversation deletion**: You can delete entire conversations via `delete item N of (get every chat whose id = chatID)`, but this removes the whole thread, not a single message.
- **Message-level scripting**: Apple has significantly restricted scripting for Messages on modern macOS. Accessing individual message properties (e.g., participants, message content) is unreliable; deleting a single sent message via AppleScript is not supported.

### JavaScript for Automation (JXA)

JXA has the same limitations as AppleScript for Messages — it uses the same Apple Event bridge.

- **No documented delete/unsend**: JXA-Cookbook and Stack Overflow show `send` for Messages but no delete/unsend operations.
- **Known reliability issues**: JXA's Apple Event implementation for Messages is flawed; silent failures are common.
- **Conclusion**: JXA cannot unsend a message. The existing human iMessage channel uses JXA for tapback (System Events), not for Messages.app scripting.

### Accessibility / UI Automation

**Feasible.** This is the only programmatic path that can trigger "Undo Send".

- **How it works**: Right-click (or control-click) a sent message → contextual menu → "Undo Send". Available for 2 minutes after sending on macOS Ventura+ / iOS 16+.
- **Implementation approach**: Use System Events (as the tapback code does) to:
  1. Activate Messages.app
  2. Navigate to the correct conversation (if multi-window)
  3. Find the last sent message row in the conversation table
  4. Invoke `AXShowMenu` on that row (or equivalent right-click)
  5. Select the "Undo Send" menu item

- **Existing precedent**: `src/channels/imessage.c` already uses JXA + System Events for tapback:
  - Activates Messages, gets `SE.processes["Messages"]`, navigates `windows()[0].tables()`, finds last row, calls `r.actions["AXShowMenu"].perform()`.
- **Risks**:
  - UI hierarchy varies by macOS version and Messages layout (sidebar vs. single pane).
  - Requires Accessibility permission (same as tapback).
  - 2-minute window is strict; any delay (e.g., polling, user confirmation) may exceed it.
  - Fragile: Apple can change the menu label or structure in updates.

### Direct Database Modification

**Not feasible for unsend.**

- **Location**: `~/Library/Messages/chat.db` (SQLite).
- **Tables**: `message`, `deleted_messages`, `sync_deleted_messages`.
- **Deleting a row**: Manually deleting a row from `message` (or inserting into `deleted_messages`/`sync_deleted_messages`) does **not** trigger the iMessage unsend protocol.
- **Effect**: Changes are local only. The message remains on the recipient's device and on Apple's servers. The Messages app may not reflect the change correctly, and sync with iCloud/other devices is undefined.
- **Conclusion**: Database modification is useful for local analysis or cleanup, but cannot achieve true "unsend" (removal from recipient and servers).

### Private Frameworks

**No documented unsend/retract API.**

- **IMDaemonCore**: Handles iMessage daemon operations; no public or reverse-engineered unsend/retract method documented.
- **IMDMessageStore / IMDPersistence**: Manages message storage and `chat.db` via `IMDPersistenceAgent` XPC. Exposes message record operations (e.g., sequence numbers) but no retract/unsend.
- **ChatKit**: Contains `CKDBMessage` and related classes; used for message data access. No documented unsend.
- **Caveats**: These are private frameworks. Using them requires `dlopen()` or similar; App Store distribution would be rejected. Reverse-engineering is incomplete and version-dependent.

### Third-Party Tools

- **photon-hq/advanced-imessage-kit** (TypeScript): Commercial SDK with `messages.unsendMessage()`. Implementation is closed-source; likely uses private frameworks or UI automation. Not suitable as a dependency for a C runtime.
- **imsg** (CLI): Uses AppleScript and public APIs; unsend not documented in free tier.
- **imsg-bridge**: REST bridge over imsg; no unsend.
- **Blooio**: Cloud iMessage API; no unsend endpoint documented.
- **delete-iMessages** (Node.js): Mass deletion via bash/SQL; local-only, not protocol unsend.

## Recommendation

**Implement unsend only via accessibility/UI automation**, and only if the product requirement justifies the fragility:

1. **Scope**: Add an optional unsend path that runs immediately after send (or within a very short, configurable window). Do not expose as a general "unsend any message" feature — the 2-minute limit makes that impractical.
2. **Implementation**: Extend the existing JXA + System Events pattern (used for tapback). After sending, optionally run a script that:
   - Activates Messages
   - Locates the last sent message in the active conversation
   - Triggers `AXShowMenu`, then selects "Undo Send" from the menu
3. **Guards**: Require Accessibility permission (same as tapback). Document that unsend is best-effort and may fail on layout changes or if the 2-minute window is exceeded.
4. **Frequency**: The requirement states &lt;0.5% frequency. Given the fragility, consider making this opt-in (e.g., config flag) rather than default-on.

**Do not** pursue: AppleScript/JXA Messages dictionary, direct `chat.db` modification for protocol unsend, or private framework usage.

## Risk Assessment

| Risk                                          | Severity | Mitigation                                                        |
| --------------------------------------------- | -------- | ----------------------------------------------------------------- |
| UI hierarchy change (macOS update)            | High     | Version checks; graceful failure; document as best-effort         |
| 2-minute window exceeded                      | Medium   | Run unsend immediately after send; avoid user confirmation delays |
| Accessibility permission denied               | Medium   | Same as tapback; clear error message                              |
| Wrong message unsent (multi-message burst)    | Medium   | Unsend only the last sent message; document ordering assumptions  |
| Messages.app not focused / wrong conversation | Medium   | Ensure correct conversation is active before unsend attempt       |
