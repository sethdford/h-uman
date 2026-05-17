---
plan: docs/plans/2026-03-03-project-obsidian.md
auditor: group-1-ui-design
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: FULL
verdict: SUPERSEDED
confidence: HIGH
---

## Plan Summary
Nuclear rewrite of chat engine with proper markdown pipeline (marked + DOMPurify
+ Shiki + KaTeX), conversational components (reasoning block, error boundary,
empty state, chat search), and progressive enhancement waves.

## Key Claims (from the plan)
- Claim 1: Markdown pipeline at `ui/src/lib/markdown.ts` using marked/DOMPurify
- Claim 2: `ui/src/components/hu-code-block.ts` with Shiki lazy loading
- Claim 3: `hu-reasoning-block`, `hu-error-boundary`, `hu-chat-search`, `hu-empty-state`
- Claim 4: marked/dompurify/shiki/katex installed in `ui/package.json`

## Evidence

### Implemented? (code exists)
- `ui/src/lib/markdown.ts` + `markdown.test.ts` exist.
- `ui/src/components/hu-code-block.ts`, `hu-reasoning-block.ts`,
  `hu-error-boundary.ts`, `hu-chat-search.ts`, `hu-empty-state.ts` all exist.
- `ui/package.json` declares: `marked ^18.0.0`, `dompurify ^3.3.3`,
  `shiki ^4.0.2`, `katex ^0.16.45`, `@types/dompurify ^3.2.0`.

### Proven? (tests exist)
- `ui/src/lib/markdown.test.ts` exists (markdown pipeline tested).
- Smoke tests for hu-thinking/hu-message-stream in extra-components.test.ts
  cover related rendering surfaces.
- No dedicated test files for hu-code-block, hu-reasoning-block,
  hu-error-boundary, or hu-chat-search located.

### Wired? (called in runtime path / dispatch)
- chat-view.ts uses ChatController + hu-message-thread (the new orchestration);
  markdown pipeline consumed via hu-message-stream/hu-message-thread.

## Gaps
- Test coverage for several new components (code-block, reasoning-block, error
  boundary, chat search) is thin or via integration only.

## Notes
Plan frontmatter is `status: superseded`. Verdict SUPERSEDED reflects that
later plans (Scalpel, Prism, SOTA overhaul) absorbed the post-Wave 1
deliverables. Wave 1 (markdown pipeline + chat rewrite) shipped substantively.
