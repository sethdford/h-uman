# Project Scalpel — Chat Page Architectural Rewrite

**Date:** 2026-03-07
**Status:** Approved
**Scope:** Full decomposition of `chat-view.ts` god object + SOTA feature additions

## Problem

`chat-view.ts` is a 1,179-line god object handling 15+ concerns: message rendering,
gateway/streaming, file upload, search, context menus, keyboard shortcuts, caching,
history, input, scroll, error handling, and more. Critical bugs exist (context menu
never rendered, dead CSS). UX gaps vs Claude.ai/ChatGPT in message actions, session
management, and file handling.

## Target Component Tree

```
hu-chat-view (~200 lines, thin orchestrator)
├── hu-chat-sessions-panel (inline session list, new chat)
├── hu-chat-search (exists)
├── hu-message-list (scroll container, auto-scroll, grouping)
│   ├── hu-message-group (grouped by sender + 2-min window)
│   │   ├── hu-message-stream (exists — content rendering)
│   │   └── hu-message-actions (hover: copy, retry, regenerate, edit)
│   ├── hu-tool-result (exists)
│   └── hu-reasoning-block (exists)
├── hu-composer (input, send, file attach, suggestions)
│   ├── hu-file-preview (attachment thumbnails)
│   └── suggested prompts (empty state)
├── hu-context-menu (exists — wire it)
└── ChatController (reactive controller — gateway, streaming, cache)
```

## New Components

### ChatController (Lit ReactiveController, ~300 lines)

Owns all non-UI logic:

- `items: ChatItem[]` — the unified message/tool/thinking array
- Gateway event handling (chunk, sent, received, thinking)
- Streaming state and timers
- Session cache (sessionStorage) and history loading
- Methods: `send()`, `abort()`, `retry()`, `regenerate()`
- Fully unit-testable without DOM

### hu-composer (~250 lines)

- Auto-resizing textarea (max 5 lines)
- File attachment button (opens file picker)
- Drag-and-drop zone with `hu-file-preview` thumbnails
- Send button (disabled when empty/disconnected/waiting)
- Character count
- Suggested prompt pills (shown in empty state)
- Events: `hu-send`, `hu-abort`, `hu-use-suggestion`

### hu-message-list (~200 lines)

- Scroll container with auto-scroll on new messages
- "New messages" pill when scrolled up
- Message grouping by sender + 2-minute time window
- Renders `hu-message-group` wrappers around consecutive same-sender messages
- Delegates to `hu-message-stream`, `hu-tool-result`, `hu-reasoning-block`

### hu-message-actions (~150 lines)

- Hover bar on each message (appears on mouseenter)
- Actions: Copy (clipboard), Retry (user), Regenerate (assistant), Edit (user)
- Phosphor icons, design token styling
- Events: `hu-copy`, `hu-retry`, `hu-regenerate`, `hu-edit`

### hu-chat-sessions-panel (~250 lines)

- Collapsible panel on left (drawer on mobile)
- Lists recent sessions with titles and timestamps
- "New Chat" button at top
- Rename and delete per session
- Active session highlighted
- Events: `hu-session-select`, `hu-session-new`, `hu-session-delete`

### hu-file-preview (~80 lines)

- Thumbnail grid for attached files
- Image preview for image types
- File icon + name for other types
- Remove button per file
- Events: `hu-file-remove`

## Fixes Included

1. **Context menu**: Wire `<hu-context-menu>` into chat-view render template
2. **Dead CSS**: Remove ~60 lines of unused `.tool-card` styles
3. **Double bubble**: Remove overlapping message styles between chat-view and hu-message-stream
4. **Adaptive code theme**: hu-code-block switches Shiki theme based on prefers-color-scheme

## Phases

### Phase 1: Controller + Bug Fixes

- Extract `ChatController` from chat-view.ts
- Wire context menu into template (fix BROKEN bug)
- Remove dead CSS and double-bubble styles
- Unit tests for ChatController
- chat-view.ts shrinks from 1,179 to ~800 lines

### Phase 2: Component Decomposition

- Extract `hu-composer` (input bar, file upload, suggestions)
- Extract `hu-message-list` (scroll, grouping, rendering)
- Rewrite `chat-view.ts` as thin orchestrator (~200 lines)
- Component tests for hu-composer and hu-message-list
- E2E tests still pass

### Phase 3: SOTA Features

- `hu-message-actions` (hover copy/retry/regenerate/edit)
- `hu-chat-sessions-panel` (inline session list + new chat)
- `hu-file-preview` (attachment thumbnails)
- Message grouping by sender + 2-min time window
- Adaptive code block theme (light/dark)
- Component tests + E2E for new features

## Success Criteria

- chat-view.ts < 250 lines
- All existing E2E tests pass
- Context menu works (right-click copy/retry)
- Per-message hover actions (copy, retry, regenerate)
- Inline session panel with new chat
- 0 dead CSS, 0 double-styling
- All new components have unit tests
