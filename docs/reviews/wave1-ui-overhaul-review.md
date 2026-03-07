---
title: Wave 1 UI Overhaul Code Review
---

# Wave 1 UI Overhaul — Critical Code Review

**Scope**: `ui/src/lib/markdown.ts`, `ui/src/components/sc-code-block.ts`, `ui/src/components/sc-message-stream.ts`, `ui/src/components/sc-latex.ts`, `ui/src/views/chat-view.ts`

**Review date**: 2026-03-07

---

## CRITICAL (Must Fix)

### 1. XSS: Link and image URLs not sanitized — `javascript:` and `data:` vectors

**File**: `ui/src/lib/markdown.ts` (lines 90–112)

**Issue**: Links and images use `link.href` and `img.href` directly from the marked lexer. Malicious markdown like `[click](javascript:alert(1))` or `![](javascript:alert(1))` would render dangerous URLs. Lit escapes HTML entities but does not block `javascript:` or `data:text/html` protocols.

**Fix**: Add URL sanitization before binding. Only allow safe protocols: `https:`, `http:`, `mailto:`, `tel:`, and relative paths. For images, allow `data:image/*` only. Reject `javascript:`, `vbscript:`, `data:text/html`, etc.

```ts
function isSafeHref(href: string): boolean {
  try {
    const u = new URL(href, "https://example.com");
    return (
      ["https:", "http:", "mailto:", "tel:"].includes(u.protocol) ||
      href.startsWith("/") ||
      href.startsWith("./")
    );
  } catch {
    return false;
  }
}

function isSafeImgSrc(src: string): boolean {
  try {
    const u = new URL(src, "https://example.com");
    if (u.protocol === "data:") return /^data:image\//i.test(src);
    return (
      ["https:", "http:"].includes(u.protocol) ||
      src.startsWith("/") ||
      src.startsWith("./")
    );
  } catch {
    return false;
  }
}
```

Then use `isSafeHref(link.href) ? link.href : "#"` and `isSafeImgSrc(img.href) ? img.href : ""` (or omit the element if invalid).

---

### 2. `unsafeHTML` without DOMPurify — Shiki and KaTeX output

**Files**: `ui/src/components/sc-code-block.ts` (line 200), `ui/src/components/sc-latex.ts` (line 75)

**Issue**: Review criteria require "No use of `unsafeHTML` without prior DOMPurify sanitization." Shiki and KaTeX output are trusted library output but should still be sanitized for defense in depth and policy compliance.

**Fix**:

- **sc-code-block.ts**: `html`${unsafeHTML(DOMPurify.sanitize(this.\_highlighted, { ALLOWED_TAGS: ["pre", "code", "span"], ALLOWED_ATTR: ["class", "style"] }))}`  
  (Adjust ALLOWED_TAGS/ALLOWED_ATTR to match Shiki’s output structure.)
- **sc-latex.ts**: `html`<span class="katex">${unsafeHTML(DOMPurify.sanitize(this.\_rendered, { /_ KaTeX-safe config _/ }))}</span>`

---

### 3. `prefers-reduced-motion` selector typo — connecting animation never disabled

**File**: `ui/src/views/chat-view.ts` (lines 317–325)

**Issue**: The media query uses `.status-dot.reconnecting` but the actual class is `.status-dot.connecting`. The connecting pulse animation is never disabled for users who prefer reduced motion.

**Fix**: Change `.status-dot.reconnecting` to `.status-dot.connecting` in the `@media (prefers-reduced-motion: reduce)` block.

---

## HIGH (Should Fix)

### 4. sc-latex: `display` changes not triggering re-render

**File**: `ui/src/components/sc-latex.ts` (lines 51–71)

**Issue**: `updated()` only checks `changed.has("latex")`. If `display` changes from `false` to `true` (or vice versa), the component does not re-render with the correct `displayMode`.

**Fix**: Include `changed.has("display")` in the condition:

```ts
if ((changed.has("latex") || changed.has("display")) && this.latex && this._loaded) {
```

---

### 5. sc-latex: Initial load when `latex` is set after mount

**File**: `ui/src/components/sc-latex.ts` (lines 27–50, 51–71)

**Issue**: If `latex` is empty at mount and set later via a property update, `connectedCallback` does nothing and `updated()` only runs when `_loaded` is true. The first load never happens.

**Fix**: In `updated()`, also handle the initial load when `latex` is set and `_loaded` is false:

```ts
override updated(changed: Map<string, unknown>): void {
  if (!changed.has("latex") && !changed.has("display")) return;
  if (!this.latex) return;
  if (!this._loaded) {
    // Initial load (same logic as connectedCallback)
    import("katex").then(/* ... */);
    return;
  }
  // Re-render when latex or display changes after load
  import("katex").then(/* ... */);
}
```

---

### 6. sc-code-block: `setTimeout` not cleared on disconnect — potential leak

**File**: `ui/src/components/sc-code-block.ts` (lines 159–172)

**Issue**: The 2s timeout for resetting `_copied` is not stored or cleared. If the component is removed before it fires, `requestUpdate()` may run on a disconnected element.

**Fix**: Store the timeout ID and clear it in `disconnectedCallback`:

```ts
private _copyTimeout: ReturnType<typeof setTimeout> | null = null;

// In _onCopy:
this._copyTimeout = setTimeout(() => { ... }, 2000);

override disconnectedCallback(): void {
  if (this._copyTimeout) clearTimeout(this._copyTimeout);
  super.disconnectedCallback();
}
```

---

### 7. DOMPurify HTML blocks: links may lack `rel="noopener noreferrer"`

**File**: `ui/src/lib/markdown.ts` (lines 194–229)

**Issue**: When sanitizing HTML blocks, links with `target="_blank"` may not get `rel="noopener noreferrer"`. DOMPurify does not add this by default.

**Fix**: Use a DOMPurify hook to add `rel="noopener noreferrer"` to all `<a>` tags:

```ts
DOMPurify.addHook("afterSanitizeAttributes", (node) => {
  if (node.tagName === "A" && node.getAttribute("target") === "_blank") {
    node.setAttribute("rel", "noopener noreferrer");
  }
});
```

(Remove the hook after sanitization if you want to avoid global state.)

---

### 8. Hardcoded duration — should use design token

**File**: `ui/src/components/sc-code-block.ts` (lines 159–172)

**Issue**: `setTimeout(..., 2000)` uses a hardcoded 2000 ms. The animation rule says to use `var(--sc-duration-*)` tokens. `--sc-duration-slow` is 350 ms; there is no 2s token, but `--sc-duration-slowest` (700 ms) or a custom token would be preferable to a raw number.

**Fix**: Use `parseInt(getComputedStyle(document.documentElement).getPropertyValue("--sc-duration-slowest").trim()) || 700` or add a `--sc-toast-duration`-style token (e.g. 2000 ms) and use that. Alternatively, use `ScToast.show`’s `duration` parameter consistently if it already uses a token.

---

## MEDIUM (Nice to Fix)

### 9. Raw pixel values in chat-view — design token violation

**File**: `ui/src/views/chat-view.ts` (lines 66–67, 179–181, 247–248, 368–369, 408, 411–412)

**Issue**: Design system requires no raw pixel values. Examples: `width: 8px; height: 8px` (status-dot), `width: 14px; height: 14px` (icons), `width: 6px; height: 6px` (typing dots).

**Fix**: Replace with tokens where they exist, e.g. `var(--sc-space-xs)` for 8px. For icon sizes, use `var(--sc-icon-sm)` or similar if available, or add tokens.

---

### 10. sc-code-block: `@property` for private state

**File**: `ui/src/components/sc-code-block.ts` (lines 37–39)

**Issue**: `_highlighted`, `_copied`, `_shikiReady` are internal state but use `@state()`. That is correct. No change needed here; this is informational.

---

### 11. Unused regex constants in markdown.ts

**File**: `ui/src/lib/markdown.ts` (lines 15–16)

**Issue**: `LATEX_DISPLAY_RE` and `LATEX_INLINE_RE` are defined but `renderTextWithLatex` uses an inline combined regex. Consider removing or using them for consistency.

---

## LOW (Nitpick)

### 12. sc-message-stream: `line-height: 1.5` in chat-view

**File**: `ui/src/views/chat-view.ts` (line 92)

**Issue**: `.message` uses `line-height: 1.5` instead of a typography token. Prefer `var(--sc-leading-relaxed)` or similar if available.

---

### 13. Inline style in chat-view

**File**: `ui/src/views/chat-view.ts` (line 577)

**Issue**: `style="display:flex;align-items:center;gap:var(--sc-space-sm);flex-wrap:wrap;"` could be moved to a class in `static styles` for consistency.

---

## Verified Compliant

| Criterion                                              | Status                                           |
| ------------------------------------------------------ | ------------------------------------------------ |
| markdown.ts uses `marked.lexer()` (AST)                | Yes (line 248)                                   |
| Code blocks delegate to `<sc-code-block>`              | Yes (lines 145–150)                              |
| Shiki lazy-loaded                                      | Yes (dynamic `import("shiki")`)                  |
| KaTeX lazy-loaded                                      | Yes (dynamic `import("katex")`)                  |
| sc-message-stream API (.content, .streaming, .role)    | Unchanged                                        |
| chat-view message handling logic                       | Unchanged                                        |
| Links have `rel="noopener noreferrer"` (hand-built)    | Yes (lines 94–95)                                |
| Links have `target="_blank"` (hand-built)              | Yes (line 94)                                    |
| DOMPurify used for HTML tokens                         | Yes (lines 116–121, 195–228)                     |
| CSS uses `--sc-*` tokens (in reviewed files)           | Mostly; some raw px in chat-view                 |
| No emoji in UI                                         | Yes                                              |
| Code blocks have `role="region"` and `aria-label`      | Yes (sc-code-block lines 184–185)                |
| Copy button has `aria-label`                           | Yes (line 192)                                   |
| Message list has `role="log"` and `aria-live="polite"` | Yes (chat-view lines 516–518)                    |
| Focus-visible with accent outline                      | Yes (sc-code-block line 91)                      |
| Shiki highlighter                                      | Uses shorthand `codeToHtml` (internal singleton) |

---

## Summary

- **3 CRITICAL** issues (XSS, unsafeHTML, reduced-motion bug)
- **5 HIGH** issues (sc-latex logic, timeout cleanup, DOMPurify hooks, hardcoded duration)
- **3 MEDIUM** issues (design tokens, minor cleanup)
- **2 LOW** nits

Address CRITICAL and HIGH items before release. The XSS vectors in links/images and the reduced-motion typo are the most urgent.
