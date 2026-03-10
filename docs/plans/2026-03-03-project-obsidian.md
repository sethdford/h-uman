# Project Obsidian — UI Overhaul Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Rewrite the Human chat engine and progressively enhance the entire UI to best-in-class quality — exceeding Apple, Anthropic Claude, and Steam in craft and polish.

**Architecture:** Nuclear rewrite of chat-view + hu-message-stream with a proper markdown rendering pipeline (marked + DOMPurify + Shiki + KaTeX). Progressive enhancement of all other surfaces with spring-based motion, glass depth, and feature completeness. Retrospective audit after each wave.

**Tech Stack:** LitElement, marked, DOMPurify, Shiki (lazy), KaTeX (lazy), Vite, Playwright

---

## Wave 1: Chat Engine Rewrite

### Task 1: Install Dependencies + Update Budget

**Files:**

- Modify: `ui/package.json`
- Modify: `ui/scripts/check-bundle-size.sh`

**Steps:**

1. `cd ui && npm install marked dompurify shiki katex`
2. `cd ui && npm install -D @types/dompurify`
3. Update `BUDGET=350` → `BUDGET=500` in check-bundle-size.sh

### Task 2: Create Markdown Pipeline

**Files:**

- Create: `ui/src/lib/markdown.ts`

Uses `marked.lexer()` to tokenize markdown, then walks the AST to generate Lit `TemplateResult` nodes directly. Code blocks delegate to `<hu-code-block>`. Inline content uses `marked.parseInline()` + DOMPurify. Handles streaming partial content (unclosed code fences).

### Task 3: Create Code Block Component

**Files:**

- Create: `ui/src/components/hu-code-block.ts`

LitElement component with `.code`, `.language`, `.onCopy` properties. Lazy-loads Shiki highlighter on first render. Shows plain code initially, re-renders with syntax highlighting. Copy button with toast feedback.

### Task 4: Rewrite Message Stream

**Files:**

- Modify: `ui/src/components/hu-message-stream.ts`

Replace hand-rolled `parseMarkdown()` + `processInline()` with `renderMarkdown()` from the new pipeline. Keep the same public API (`.content`, `.streaming`, `.role`). Add glass bubble styling for assistant messages.

### Task 5: Rewrite Chat View

**Files:**

- Modify: `ui/src/views/chat-view.ts`

Replace inline empty state (lines 796-813) with `<hu-empty-state>` + suggested prompt pills. Add `role="log"` and `aria-live="polite"` to message list. Remove hardcoded branch indicators. Clean up unused CSS (.md-blockquote, .md-table styles now handled by hu-message-stream).

### Task 6: Tests

Write tests for markdown.ts, hu-code-block, updated hu-message-stream, updated chat-view.

### Task 7: Retrospective

Build, test, lint:strict, a11y audit, bundle size check, screenshot comparison.

---

## Wave 2: Streaming + Reasoning

### Task 8: Reasoning Block Component

- Create `ui/src/components/hu-reasoning-block.ts` — collapsible chain-of-thought with expand/collapse animation

### Task 9: Progressive Streaming

- Enhance hu-message-stream for progressive character reveal during streaming
- Integrate reasoning blocks when model returns thinking content
- Move tool calls inline in conversation flow

### Task 10: Retrospective

---

## Wave 3: Motion Overhaul

### Task 11: Spring-Based Interactions

- Wire `--hu-spring-*` tokens to button presses, card hovers, modal entrances
- Staggered message entrance animations
- Parallax hover on cards

### Task 12: Glass Depth

- Multiple glass layers with dynamic tint
- Pointer-responsive refraction via dynamic-light.ts
- Sidebar, message bubbles, and modals get depth layers

### Task 13: Retrospective

---

## Wave 4: Feature Completeness

### Task 14: Chat Features

- Real message branching (wire hu-message-branch to conversation data)
- Chat search (hu-chat-search component)
- Keyboard shortcut overlay (? key)
- Context menus on messages (right-click)
- File upload via drag-and-drop

### Task 15: Retrospective

---

## Wave 5: Polish + Ship

### Task 16: Error Boundaries + Onboarding

- hu-error-boundary component for graceful crash recovery
- First-run onboarding flow
- Performance profiling and optimization

### Task 17: Final Retrospective + Ship
