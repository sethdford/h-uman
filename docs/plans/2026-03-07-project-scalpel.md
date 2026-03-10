# Project Scalpel — Chat Page Rewrite Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Decompose the 1,179-line chat-view.ts god object into 6 focused components + a reactive controller, fix critical bugs, and add SOTA features matching Claude.ai/ChatGPT UX.

**Architecture:** Extract a `ChatController` (Lit ReactiveController) that owns all gateway/streaming/cache logic. Decompose rendering into `hu-composer` (input), `hu-message-list` (scroll + rendering), `hu-message-actions` (hover toolbar). Add `hu-chat-sessions-panel` for inline session management. chat-view.ts becomes a ~200-line orchestrator.

**Tech Stack:** Lit 3, TypeScript, Vitest, Playwright, `--hu-*` design tokens, Phosphor icons

---

## Phase 1: Controller + Bug Fixes

### Task 1: Extract ChatController

**Files:**

- Create: `ui/src/controllers/chat-controller.ts`
- Create: `ui/src/controllers/chat-controller.test.ts`
- Reference: `ui/src/views/chat-view.ts` (source of logic to extract)
- Reference: `ui/src/gateway-aware.ts` (base class pattern)

**Step 1: Write the test file**

Create `ui/src/controllers/chat-controller.test.ts`:

```typescript
import { describe, it, expect, vi, beforeEach } from "vitest";
import { ChatController, type ChatItem } from "./chat-controller.js";

function makeMockHost() {
  return {
    addController: vi.fn(),
    removeController: vi.fn(),
    requestUpdate: vi.fn(),
    updateComplete: Promise.resolve(true),
  };
}

function makeMockGateway() {
  return {
    request: vi.fn().mockResolvedValue({}),
    abort: vi.fn().mockResolvedValue(undefined),
    status: "connected" as const,
  };
}

describe("ChatController", () => {
  let host: ReturnType<typeof makeMockHost>;
  let gateway: ReturnType<typeof makeMockGateway>;
  let ctrl: ChatController;

  beforeEach(() => {
    host = makeMockHost();
    gateway = makeMockGateway();
    ctrl = new ChatController(host as any, () => gateway as any);
    sessionStorage.clear();
  });

  it("initializes with empty items", () => {
    expect(ctrl.items).toEqual([]);
    expect(ctrl.isWaiting).toBe(false);
    expect(ctrl.streamElapsed).toBe("");
  });

  it("send() appends user message and calls gateway", async () => {
    await ctrl.send("hello", "session-1");
    expect(ctrl.items).toHaveLength(1);
    expect(ctrl.items[0]).toMatchObject({
      type: "message",
      role: "user",
      content: "hello",
    });
    expect(gateway.request).toHaveBeenCalledWith("chat.send", {
      message: "hello",
      sessionKey: "session-1",
    });
  });

  it("send() sets isWaiting and starts stream timer", async () => {
    await ctrl.send("test", "s1");
    expect(ctrl.isWaiting).toBe(true);
  });

  it("send() stores last failed message on error", async () => {
    gateway.request.mockRejectedValueOnce(new Error("fail"));
    await ctrl.send("test", "s1");
    expect(ctrl.isWaiting).toBe(false);
    expect(ctrl.lastFailedMessage).toBe("test");
  });

  it("handleEvent processes chunk events", () => {
    ctrl.items = [{ type: "message", role: "assistant", content: "Hi", ts: 1 }];
    ctrl.handleEvent("chat", { state: "chunk", message: " there" });
    expect(ctrl.items[0]).toMatchObject({ content: "Hi there" });
  });

  it("handleEvent processes sent events", () => {
    ctrl.handleEvent("chat", { state: "sent", message: "Done" });
    expect(
      ctrl.items.some((i) => i.type === "message" && i.role === "assistant"),
    ).toBe(true);
    expect(ctrl.isWaiting).toBe(false);
  });

  it("handleEvent processes tool_call events", () => {
    ctrl.handleEvent("tool_call", { id: "t1", message: "shell", result: "ok" });
    expect(ctrl.items).toHaveLength(1);
    expect(ctrl.items[0]).toMatchObject({
      type: "tool_call",
      id: "t1",
      name: "shell",
    });
  });

  it("handleEvent processes thinking events", () => {
    ctrl.handleEvent("thinking", { message: "Hmm..." });
    expect(ctrl.items).toHaveLength(1);
    expect(ctrl.items[0]).toMatchObject({
      type: "thinking",
      content: "Hmm...",
    });
  });

  it("abort() calls gateway.abort and clears waiting", async () => {
    ctrl.isWaiting = true;
    await ctrl.abort();
    expect(gateway.abort).toHaveBeenCalled();
    expect(ctrl.isWaiting).toBe(false);
  });

  it("retry() re-sends last failed message", async () => {
    ctrl.lastFailedMessage = "retry me";
    await ctrl.retry("s1");
    expect(ctrl.items).toHaveLength(1);
    expect(ctrl.items[0]).toMatchObject({ content: "retry me" });
    expect(ctrl.lastFailedMessage).toBe("");
  });

  it("caches and restores messages", () => {
    ctrl.items = [{ type: "message", role: "user", content: "cached", ts: 1 }];
    ctrl.cacheMessages("s1");
    ctrl.items = [];
    const restored = ctrl.restoreFromCache("s1");
    expect(restored).toBe(true);
    expect(ctrl.items).toHaveLength(1);
  });

  it("loadHistory fetches from gateway", async () => {
    gateway.request.mockResolvedValueOnce({
      messages: [{ role: "user", content: "hi" }],
    });
    await ctrl.loadHistory("s1");
    expect(ctrl.items).toHaveLength(1);
  });
});
```

**Step 2: Run test to verify it fails**

Run: `cd ui && npx vitest run src/controllers/chat-controller.test.ts`
Expected: FAIL with "Cannot find module"

**Step 3: Write ChatController implementation**

Create `ui/src/controllers/chat-controller.ts`:

```typescript
import type { ReactiveController, ReactiveControllerHost } from "lit";

export type ChatItem =
  | {
      type: "message";
      role: "user" | "assistant";
      content: string;
      id?: string;
      ts?: number;
    }
  | {
      type: "tool_call";
      id: string;
      name: string;
      input?: string;
      status: "running" | "completed";
      result?: string;
      ts?: number;
    }
  | {
      type: "thinking";
      content: string;
      streaming: boolean;
      duration?: string;
      ts?: number;
    };

export interface GatewayLike {
  request<T = unknown>(
    method: string,
    params?: Record<string, unknown>,
  ): Promise<T>;
  abort(): Promise<void>;
  status: string;
}

export class ChatController implements ReactiveController {
  items: ChatItem[] = [];
  isWaiting = false;
  lastFailedMessage = "";
  errorBanner = "";
  streamElapsed = "";

  private _streamStartTime = 0;
  private _streamTimer = 0;
  private _host: ReactiveControllerHost;
  private _getGateway: () => GatewayLike | null;

  constructor(
    host: ReactiveControllerHost,
    getGateway: () => GatewayLike | null,
  ) {
    this._host = host;
    this._getGateway = getGateway;
    host.addController(this);
  }

  hostConnected(): void {}
  hostDisconnected(): void {
    this._stopStreamTimer();
  }

  async send(text: string, sessionKey: string): Promise<void> {
    const gw = this._getGateway();
    if (!text.trim() || !gw) return;
    this.items = [
      ...this.items,
      { type: "message", role: "user", content: text.trim(), ts: Date.now() },
    ];
    this.lastFailedMessage = "";
    this.isWaiting = true;
    this._startStreamTimer();
    this._host.requestUpdate();
    try {
      await gw.request("chat.send", { message: text.trim(), sessionKey });
    } catch (err) {
      this.isWaiting = false;
      this._stopStreamTimer();
      this.lastFailedMessage = text.trim();
      this._host.requestUpdate();
      throw err;
    }
  }

  async abort(): Promise<void> {
    try {
      await this._getGateway()?.abort();
    } catch {
      /* best-effort */
    }
    this.isWaiting = false;
    this._stopStreamTimer();
    this._host.requestUpdate();
  }

  async retry(sessionKey: string): Promise<void> {
    if (!this.lastFailedMessage) return;
    const msg = this.lastFailedMessage;
    this.lastFailedMessage = "";
    await this.send(msg, sessionKey);
  }

  async loadHistory(sessionKey: string): Promise<void> {
    const gw = this._getGateway();
    if (!gw) return;
    try {
      const res = await gw.request<{
        messages?: { role: string; content: string }[];
      }>("chat.history", { sessionKey });
      if (res?.messages?.length) {
        this.items = res.messages.map((m) => ({
          type: "message" as const,
          role: m.role as "user" | "assistant",
          content: m.content ?? "",
        }));
        this.cacheMessages(sessionKey);
        this._host.requestUpdate();
        return;
      }
    } catch {
      /* best-effort */
    }
    this.restoreFromCache(sessionKey);
  }

  handleEvent(event: string, payload: Record<string, unknown>): void {
    if (event === "error") {
      this.errorBanner = String(
        payload.message ?? payload.error ?? "Unknown error",
      );
      this._host.requestUpdate();
      return;
    }

    if (
      event === "thinking" ||
      (event === "chat" && payload.state === "thinking")
    ) {
      const content = String(payload.message ?? "");
      const existing = this.items.filter(
        (i): i is Extract<ChatItem, { type: "thinking" }> =>
          i.type === "thinking" && i.streaming,
      );
      const last = existing[existing.length - 1];
      if (last) {
        this.items = this.items.map((i) =>
          i === last ? { ...i, content: i.content + content } : i,
        );
      } else {
        this.items = [
          ...this.items,
          { type: "thinking", content, streaming: true, ts: Date.now() },
        ];
      }
      this._host.requestUpdate();
      return;
    }

    if (event === "chat") {
      const state = payload.state as string;
      const content = String(payload.message ?? "");

      if (state === "received" && content) {
        const recentUser = this.items
          .slice(-6)
          .some(
            (i) =>
              i.type === "message" &&
              i.role === "user" &&
              i.content === content,
          );
        if (!recentUser) {
          this.items = [
            ...this.items,
            {
              type: "message",
              role: "user",
              content,
              id: payload.id as string,
              ts: Date.now(),
            },
          ];
        }
      }

      if (state === "sent" && content) {
        this.items = this.items.map((i) =>
          i.type === "thinking" && i.streaming ? { ...i, streaming: false } : i,
        );
        this.items = [
          ...this.items,
          {
            type: "message",
            role: "assistant",
            content,
            id: payload.id as string,
            ts: Date.now(),
          },
        ];
        this.isWaiting = false;
        this._stopStreamTimer();
      }

      if (state === "chunk" && content) {
        this.items = this.items.map((i) =>
          i.type === "thinking" && i.streaming ? { ...i, streaming: false } : i,
        );
        let lastIdx = -1;
        for (let i = this.items.length - 1; i >= 0; i--) {
          if (
            this.items[i].type === "message" &&
            (this.items[i] as any).role === "assistant"
          ) {
            lastIdx = i;
            break;
          }
        }
        if (lastIdx >= 0) {
          const last = this.items[lastIdx];
          if (last.type === "message") {
            this.items = [
              ...this.items.slice(0, lastIdx),
              { ...last, content: last.content + content },
              ...this.items.slice(lastIdx + 1),
            ];
          }
        } else {
          this.items = [
            ...this.items,
            {
              type: "message",
              role: "assistant",
              content,
              id: payload.id as string,
              ts: Date.now(),
            },
          ];
        }
      }

      if (state === "sent" && !content) {
        this.isWaiting = true;
        this._startStreamTimer();
      }

      this._host.requestUpdate();
      return;
    }

    if (event === "tool_call") {
      const id = String(payload.id ?? `tool-${Date.now()}`);
      const name = String(payload.message ?? "tool");
      const input =
        typeof payload.input === "string"
          ? payload.input
          : payload.args != null
            ? JSON.stringify(payload.args)
            : undefined;
      const result =
        payload.result != null ? String(payload.result) : undefined;
      const existingIdx = this.items.findIndex(
        (i): i is Extract<ChatItem, { type: "tool_call" }> =>
          i.type === "tool_call" && i.id === id,
      );
      if (existingIdx < 0) {
        this.items = [
          ...this.items,
          {
            type: "tool_call",
            id,
            name,
            input,
            status: result != null ? "completed" : "running",
            result,
            ts: Date.now(),
          },
        ];
      } else {
        const existing = this.items[existingIdx];
        if (existing.type === "tool_call") {
          this.items = [
            ...this.items.slice(0, existingIdx),
            {
              ...existing,
              input: existing.input ?? input,
              status: "completed",
              result: result ?? existing.result,
            },
            ...this.items.slice(existingIdx + 1),
          ];
        }
      }
      this._host.requestUpdate();
      return;
    }
  }

  cacheMessages(sessionKey: string): void {
    try {
      sessionStorage.setItem(
        `hu-chat-${sessionKey}`,
        JSON.stringify(this.items),
      );
    } catch {
      /* quota exceeded */
    }
  }

  restoreFromCache(sessionKey: string): boolean {
    try {
      const raw = sessionStorage.getItem(`hu-chat-${sessionKey}`);
      if (!raw) return false;
      const cached = JSON.parse(raw) as unknown;
      if (!Array.isArray(cached) || cached.length === 0) return false;
      this.items = cached
        .map((item: unknown) => {
          const obj = item as Record<string, unknown>;
          if (
            obj?.type === "message" ||
            obj?.type === "tool_call" ||
            obj?.type === "thinking"
          )
            return item as ChatItem;
          if (obj?.role && obj?.content)
            return {
              type: "message",
              role: obj.role,
              content: String(obj.content ?? ""),
            } as ChatItem;
          return null;
        })
        .filter((i): i is ChatItem => i != null);
      this._host.requestUpdate();
      return this.items.length > 0;
    } catch {
      return false;
    }
  }

  private _startStreamTimer(): void {
    this._streamStartTime = Date.now();
    this.streamElapsed = "0s";
    this._streamTimer = window.setInterval(() => {
      const elapsed = Math.floor((Date.now() - this._streamStartTime) / 1000);
      this.streamElapsed =
        elapsed < 60
          ? `${elapsed}s`
          : `${Math.floor(elapsed / 60)}m ${elapsed % 60}s`;
      this._host.requestUpdate();
    }, 1000);
  }

  private _stopStreamTimer(): void {
    if (this._streamTimer) {
      window.clearInterval(this._streamTimer);
      this._streamTimer = 0;
    }
    this.streamElapsed = "";
  }
}
```

**Step 4: Run tests to verify they pass**

Run: `cd ui && npx vitest run src/controllers/chat-controller.test.ts`
Expected: All tests PASS

**Step 5: Commit**

```bash
git add ui/src/controllers/chat-controller.ts ui/src/controllers/chat-controller.test.ts
git commit -m "feat(chat): extract ChatController reactive controller from chat-view

Moves all gateway event handling, streaming state, caching, history,
send/abort/retry logic into a standalone Lit ReactiveController.
Fully unit-testable without DOM. 13 tests."
```

---

### Task 2: Fix context menu (BROKEN) and remove dead CSS

**Files:**

- Modify: `ui/src/views/chat-view.ts`

**Step 1: Wire context menu into render template**

In `chat-view.ts`, add context menu rendering at the end of the `render()` method, just before the closing `</div>`:

Find the `render()` method return, and after `${this._renderInputBar()}` add:

```typescript
${this._contextMenu.open
  ? html`
      <hu-context-menu
        .open=${this._contextMenu.open}
        .x=${this._contextMenu.x}
        .y=${this._contextMenu.y}
        .items=${this._contextMenu.items}
        @close=${() => (this._contextMenu = { ...this._contextMenu, open: false })}
      ></hu-context-menu>
    `
  : nothing}
```

**Step 2: Remove dead CSS**

Remove all `.tool-card`, `.tool-header`, `.tool-expand`, `.tool-spinner`, `.tool-body` CSS rules (approximately lines 235-298 in the `static styles` block). These classes are not used anywhere in the template.

**Step 3: Run tests**

Run: `cd ui && npm run typecheck && npm run test && npx playwright test e2e/chat.spec.ts`
Expected: All pass

**Step 4: Commit**

```bash
git add ui/src/views/chat-view.ts
git commit -m "fix(chat): wire context menu into template, remove dead CSS

Context menu was set in _onMessageContextMenu but hu-context-menu
was never rendered. Right-click copy/retry now works.
Removed ~60 lines of unused .tool-card CSS."
```

---

### Task 3: Wire ChatController into chat-view.ts

**Files:**

- Modify: `ui/src/views/chat-view.ts`

**Step 1: Import and instantiate controller**

Replace the direct state management in chat-view.ts with the ChatController:

1. Import: `import { ChatController, type ChatItem } from "../controllers/chat-controller.js";`
2. Remove the local `ChatItem` type definition (lines 18-41)
3. Add controller: `private chat = new ChatController(this, () => this.gateway as any);`
4. Remove state properties that are now in the controller: `items`, `isWaiting`, `errorBanner`, `lastFailedMessage`, `_streamElapsed`, `_streamStartTime`, `_streamTimer`
5. Keep state properties that are view-only: `inputValue`, `connectionStatus`, `showScrollPill`, `_searchOpen`, `_searchQuery`, `_searchCurrentMatch`, `_dragOver`, `_contextMenu`
6. Replace `this.items` with `this.chat.items` throughout
7. Replace `this.isWaiting` with `this.chat.isWaiting` throughout
8. Replace `this.errorBanner` with `this.chat.errorBanner` throughout
9. Replace `this._streamElapsed` with `this.chat.streamElapsed` throughout
10. Replace `this.lastFailedMessage` with `this.chat.lastFailedMessage` throughout
11. Replace `onGatewayMessage` body with delegation: `this.chat.handleEvent(detail.event, payload)`
12. Replace `send()` with: call `this.chat.send(text, this.sessionKey)` + scroll + resize
13. Replace `handleAbort()` with: `this.chat.abort()`
14. Replace `loadHistory()` with: `this.chat.loadHistory(this.sessionKey)`
15. Remove `_cacheMessages`, `_restoreFromCache`, `_cacheKey`, `_startStreamTimer`, `_stopStreamTimer` methods
16. Remove the `load()` override and use: `protected override async load() { await this.chat.loadHistory(this.sessionKey); }`

**Step 2: Run all tests**

Run: `cd ui && npm run typecheck && npm run test && npx playwright test`
Expected: All pass

**Step 3: Commit**

```bash
git add ui/src/views/chat-view.ts
git commit -m "refactor(chat): wire ChatController, remove 300+ lines of inline logic

chat-view.ts now delegates all gateway/streaming/cache logic to
ChatController. View is now ~800 lines (down from 1,179)."
```

---

## Phase 2: Component Decomposition

### Task 4: Extract hu-composer

**Files:**

- Create: `ui/src/components/hu-composer.ts`
- Create: `ui/src/components/hu-composer.test.ts`
- Modify: `ui/src/views/chat-view.ts`

**Step 1: Write test**

Create `ui/src/components/hu-composer.test.ts`:

```typescript
import { describe, it, expect, vi } from "vitest";
import "./hu-composer.js";

describe("hu-composer", () => {
  it("registers as custom element", () => {
    expect(customElements.get("hu-composer")).toBeDefined();
  });

  it("renders textarea and send button", async () => {
    const el = document.createElement("hu-composer") as HTMLElement & {
      updateComplete: Promise<boolean>;
    };
    document.body.appendChild(el);
    await el.updateComplete;
    const textarea = el.shadowRoot?.querySelector("textarea");
    const sendBtn = el.shadowRoot?.querySelector(".send-btn");
    expect(textarea).toBeTruthy();
    expect(sendBtn).toBeTruthy();
    el.remove();
  });

  it("disables send when empty", async () => {
    const el = document.createElement("hu-composer") as any;
    document.body.appendChild(el);
    await el.updateComplete;
    const sendBtn = el.shadowRoot?.querySelector(
      ".send-btn",
    ) as HTMLButtonElement;
    expect(sendBtn?.disabled).toBe(true);
    el.remove();
  });

  it("fires hu-send event on Enter", async () => {
    const el = document.createElement("hu-composer") as any;
    el.value = "hello";
    document.body.appendChild(el);
    await el.updateComplete;
    const sent = vi.fn();
    el.addEventListener("hu-send", sent);
    const textarea = el.shadowRoot?.querySelector(
      "textarea",
    ) as HTMLTextAreaElement;
    textarea.dispatchEvent(
      new KeyboardEvent("keydown", { key: "Enter", bubbles: true }),
    );
    expect(sent).toHaveBeenCalled();
    el.remove();
  });

  it("shows file attachment button", async () => {
    const el = document.createElement("hu-composer") as any;
    document.body.appendChild(el);
    await el.updateComplete;
    const attachBtn = el.shadowRoot?.querySelector(".attach-btn");
    expect(attachBtn).toBeTruthy();
    el.remove();
  });

  it("renders suggested prompts when showSuggestions is true", async () => {
    const el = document.createElement("hu-composer") as any;
    el.showSuggestions = true;
    document.body.appendChild(el);
    await el.updateComplete;
    const pills = el.shadowRoot?.querySelectorAll(".prompt-pill") ?? [];
    expect(pills.length).toBeGreaterThan(0);
    el.remove();
  });
});
```

**Step 2: Implement hu-composer**

Create `ui/src/components/hu-composer.ts`. This component handles:

- Auto-resizing textarea
- Send button (disabled when empty/waiting/disconnected)
- File attachment button (opens file picker)
- Drag-and-drop zone
- Suggested prompt pills (when `showSuggestions` property is true)
- Character count
- Events: `hu-send` (detail: { message: string }), `hu-abort`, `hu-use-suggestion` (detail: { text: string }), `hu-files` (detail: { files: File[] })

Properties: `value`, `waiting`, `disabled`, `showSuggestions`, `streamElapsed`

**Step 3: Wire into chat-view**

Replace `_renderInputBar()`, `_renderEmptyState()` prompt pills, `handleInput()`, `handleKeyDown()`, `resizeTextarea()`, drag-and-drop handlers with `<hu-composer>`.

**Step 4: Run tests**

Run: `cd ui && npm run typecheck && npm run test && npx playwright test`

**Step 5: Commit**

```bash
git add ui/src/components/hu-composer.ts ui/src/components/hu-composer.test.ts ui/src/views/chat-view.ts
git commit -m "feat(chat): extract hu-composer — input bar, file attach, suggestions

Moves textarea, send button, file attachment, drag-drop, and prompt
pills into a reusable hu-composer component. chat-view.ts shrinks
by ~150 lines."
```

---

### Task 5: Extract hu-message-list

**Files:**

- Create: `ui/src/components/hu-message-list.ts`
- Create: `ui/src/components/hu-message-list.test.ts`
- Modify: `ui/src/views/chat-view.ts`

**Step 1: Write test**

Create `ui/src/components/hu-message-list.test.ts` with tests for:

- Custom element registration
- Renders messages from `items` array
- Has `role="log"` and `aria-live="polite"`
- Shows scroll-to-bottom pill when not at bottom
- Groups consecutive same-role messages within 2-minute window
- Renders `hu-message-stream` for message items
- Renders `hu-tool-result` for tool_call items
- Renders `hu-reasoning-block` for thinking items

**Step 2: Implement hu-message-list**

Properties: `items: ChatItem[]`, `isWaiting`, `streamElapsed`
Events: `hu-scroll-bottom`, `hu-context-menu` (detail: { event, item }), `hu-abort`
Internal: scroll handler, message grouping logic, stagger animation

**Step 3: Wire into chat-view**

Replace the `#message-list` div, `_renderMessages()`, `_renderThinking()`, `_renderScrollPill()`, `scrollToBottom()`, `_scrollHandler`, scroll state with `<hu-message-list>`.

**Step 4: Run tests and commit**

---

### Task 6: Rewrite chat-view.ts as thin orchestrator

**Files:**

- Modify: `ui/src/views/chat-view.ts`

After Tasks 3-5, chat-view.ts should be ~200 lines:

- Imports and registers sub-components
- Owns `ChatController`
- Owns session key, connection status, search state, context menu state
- Renders: `<hu-chat-search>`, `<hu-message-list>`, `<hu-composer>`, `<hu-context-menu>`, status bar, error banner
- Wires events between components

**Step 1: Verify final line count < 300**

**Step 2: Run all tests**

Run: `cd ui && npm run typecheck && npm run test && npm run lint && npx playwright test`

**Step 3: Commit**

```bash
git commit -m "refactor(chat): chat-view.ts is now a thin orchestrator (~200 lines)

Decomposed from 1,179-line god object into:
- ChatController (gateway/streaming/cache)
- hu-composer (input/send/file attach)
- hu-message-list (scroll/rendering/grouping)
All tests pass."
```

---

## Phase 3: SOTA Features

### Task 7: Add hu-message-actions

**Files:**

- Create: `ui/src/components/hu-message-actions.ts`
- Create: `ui/src/components/hu-message-actions.test.ts`
- Modify: `ui/src/components/hu-message-list.ts` (integrate)

Hover toolbar on each message with:

- Copy (clipboard)
- Retry (user messages)
- Regenerate (assistant messages)
- Edit (user messages — replaces content)

Uses Phosphor icons: `copy`, `arrow-clockwise`, `refresh`, `file-text`
Events: `hu-copy`, `hu-retry`, `hu-regenerate`, `hu-edit`

---

### Task 8: Add hu-chat-sessions-panel

**Files:**

- Create: `ui/src/components/hu-chat-sessions-panel.ts`
- Create: `ui/src/components/hu-chat-sessions-panel.test.ts`
- Modify: `ui/src/views/chat-view.ts` (integrate)

Collapsible panel (left side on desktop, drawer on mobile):

- "New Chat" button at top
- Session list with titles, timestamps, active highlight
- Rename and delete per session
- Events: `hu-session-select`, `hu-session-new`, `hu-session-rename`, `hu-session-delete`

---

### Task 9: Add hu-file-preview + real file attachment

**Files:**

- Create: `ui/src/components/hu-file-preview.ts`
- Modify: `ui/src/components/hu-composer.ts` (integrate)

Thumbnail grid:

- Image files show preview (FileReader.readAsDataURL)
- Other files show icon + name + size
- Remove button per file
- Events: `hu-file-remove`

---

### Task 10: Message grouping + adaptive code theme

**Files:**

- Modify: `ui/src/components/hu-message-list.ts`
- Modify: `ui/src/components/hu-code-block.ts`

1. Message grouping: consecutive same-sender messages within 2-minute window share a group container. Only first message shows role indicator; subsequent messages are "connected" visually.

2. Adaptive code theme: hu-code-block detects `prefers-color-scheme` and uses `github-dark-default` or `github-light-default` accordingly.

---

### Task 11: Final validation + visual regression update

**Step 1: Run full test suite**

```bash
cd ui && npm run typecheck && npm run test && npm run lint && npm run lint:strict && npx playwright test
```

**Step 2: Update visual regression baselines**

```bash
cd ui && npx playwright test e2e/visual.spec.ts --update-snapshots
```

**Step 3: Run C tests**

```bash
cd /Users/sethford/Documents/nullclaw && cmake --build build -j$(sysctl -n hw.ncpu) && ./build/human_tests
```

**Step 4: Final commit and push**

```bash
git add -A
git commit -m "feat: Project Scalpel — chat page architectural rewrite

Decomposed 1,179-line god object into 6 focused components:
- ChatController: gateway/streaming/cache (ReactiveController)
- hu-composer: input bar, file attachment, suggestions
- hu-message-list: scroll container, message grouping, rendering
- hu-message-actions: hover copy/retry/regenerate/edit
- hu-chat-sessions-panel: inline session list + new chat
- hu-file-preview: attachment thumbnails

Fixed: context menu never rendered, dead CSS, double bubble styling.
Added: per-message hover actions, inline sessions, file previews,
message grouping, adaptive code themes."
git push
```
