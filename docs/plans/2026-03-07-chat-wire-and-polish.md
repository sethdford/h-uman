# Chat SOTA — Wire & Polish

Date: 2026-03-07
Branch: `feat/sessions-sota-redesign`

## Goal

Close the gap between existing chat components and a SOTA messaging experience.
The component library is already rich; the work is wiring and polish.

## Changes

### 1. Wire delivery status

Set `status` on user messages in `ChatController`:

- `sending` on `chat.send()`
- `sent` on first assistant response or ack
- `failed` on error

`sc-delivery-status` is already rendered by `sc-message-thread` when status is set.

### 2. Wire grouped timestamps

Show centered timestamp dividers between message groups separated by >5 minutes.
`sc-message-thread` already has time divider logic; ensure it's active and styled.

### 3. Polish chat-view layout

- Glassmorphic status bar matching overview aesthetic
- Spring entrance animations on new messages (staggered)
- Refined spacing: `--sc-space-lg` between sender groups, `--sc-space-xs` within
- Smooth scroll-to-bottom on new message

### 4. Wire retry on failed sends

- Show retry affordance on failed user messages
- Controller: `retry(messageId)` removes failed message and resends

### 5. Clean up dead code

Remove unused components replaced by `sc-message-thread`:

- `sc-message-list.ts`
- `sc-message-stream.ts`

Update imports in any file that references them.

## Out of scope (YAGNI)

Reply threading, edit/delete, forward, read receipts, branch navigation,
rich attachment galleries, offline queue.
