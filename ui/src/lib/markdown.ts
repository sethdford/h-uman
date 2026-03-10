import { html, nothing } from "lit";
import type { TemplateResult } from "lit";
import { unsafeHTML } from "lit/directives/unsafe-html.js";
import { marked, type Token, type Tokens } from "marked";
import DOMPurify from "dompurify";

import "../components/hu-code-block.js";
import "../components/hu-latex.js";

export interface RenderOptions {
  onCopyCode?: (code: string) => void;
  onImageClick?: (src: string) => void;
  streaming?: boolean;
}

function renderTextWithLatex(text: string): TemplateResult {
  if (!/\$/.test(text)) return html`${text}`;
  const combined = /\$\$[\s\S]*?\$\$|\$[^$\n]+?\$/g;
  const segments: Array<
    { type: "text"; value: string } | { type: "latex"; value: string; display: boolean }
  > = [];
  let lastIndex = 0;
  let m;
  while ((m = combined.exec(text)) !== null) {
    if (m.index > lastIndex) {
      segments.push({ type: "text", value: text.slice(lastIndex, m.index) });
    }
    const latex = m[0];
    const isDisplay = latex.startsWith("$$");
    const inner = isDisplay ? latex.slice(2, -2) : latex.slice(1, -1);
    segments.push({ type: "latex", value: inner, display: isDisplay });
    lastIndex = combined.lastIndex;
  }
  if (lastIndex < text.length) {
    segments.push({ type: "text", value: text.slice(lastIndex) });
  }
  if (segments.length === 0) return html`${text}`;
  if (segments.length === 1 && segments[0].type === "text") return html`${segments[0].value}`;
  return html`${segments.map((s) =>
    s.type === "text"
      ? html`${s.value}`
      : html`<hu-latex .latex=${s.value} .display=${s.display}></hu-latex>`,
  )}`;
}

function normalizeForParsing(text: string, streaming: boolean): string {
  if (!streaming) return text;
  const fenceCount = (text.match(/```/g) || []).length;
  if (fenceCount % 2 !== 0) return text + "\n```";
  return text;
}

export function renderInlineTokens(
  tokens: Token[] | undefined,
  options?: RenderOptions,
): TemplateResult {
  if (!tokens || tokens.length === 0) return html``;
  const parts: TemplateResult[] = [];
  for (const t of tokens) {
    switch (t.type) {
      case "text":
        parts.push(renderTextWithLatex((t as Tokens.Text).text));
        break;
      case "escape":
        parts.push(html`${(t as Tokens.Escape).text}`);
        break;
      case "codespan":
        parts.push(html`<code class="inline">${(t as Tokens.Codespan).text}</code>`);
        break;
      case "br":
        parts.push(html`<br />`);
        break;
      case "strong": {
        const strong = t as Tokens.Strong;
        parts.push(html`<strong>${renderInlineTokens(strong.tokens, options)}</strong>`);
        break;
      }
      case "em": {
        const em = t as Tokens.Em;
        parts.push(html`<em>${renderInlineTokens(em.tokens, options)}</em>`);
        break;
      }
      case "del": {
        const del = t as Tokens.Del;
        parts.push(html`<del>${renderInlineTokens(del.tokens, options)}</del>`);
        break;
      }
      case "link": {
        const link = t as Tokens.Link;
        parts.push(
          html`<a
            href="${link.href}"
            target="_blank"
            rel="noopener noreferrer"
            ${link.title ? html` title="${link.title}"` : nothing}
            >${renderInlineTokens(link.tokens, options)}</a
          >`,
        );
        break;
      }
      case "image": {
        const img = t as Tokens.Image;
        const imgEl = html`<img
          src="${img.href}"
          alt="${img.text}"
          loading="lazy"
          style="max-width: 100%; border-radius: var(--hu-radius-md);"
          class="md-image-img"
          ${img.title ? html` title="${img.title}"` : nothing}
        />`;
        if (options?.onImageClick) {
          parts.push(
            html`<span
              class="md-image-clickable"
              role="button"
              tabindex="0"
              @click=${() => options?.onImageClick?.(img.href)}
              @keydown=${(e: KeyboardEvent) => {
                if (e.key === "Enter" || e.key === " ") {
                  e.preventDefault();
                  options?.onImageClick?.(img.href);
                }
              }}
            >
              ${imgEl}
            </span>`,
          );
        } else {
          parts.push(imgEl);
        }
        break;
      }
      case "html":
        parts.push(
          html`${unsafeHTML(
            DOMPurify.sanitize((t as Tokens.HTML).text, {
              ALLOWED_TAGS: ["strong", "em", "code", "a", "br", "del", "sub", "sup", "img", "span"],
              ALLOWED_ATTR: ["href", "target", "rel", "class", "src", "alt", "loading"],
            }),
          )}`,
        );
        break;
      default:
        if ("text" in t && typeof (t as { text?: string }).text === "string") {
          parts.push(html`${(t as { text: string }).text}`);
        }
        break;
    }
  }
  return html`${parts}`;
}

function renderToken(token: Token, options?: RenderOptions): TemplateResult | typeof nothing {
  switch (token.type) {
    case "heading": {
      const h = token as Tokens.Heading;
      const content = renderInlineTokens(h.tokens, options);
      const cls = "md-heading";
      switch (h.depth) {
        case 1:
          return html`<h1 class=${cls}>${content}</h1>`;
        case 2:
          return html`<h2 class=${cls}>${content}</h2>`;
        case 3:
          return html`<h3 class=${cls}>${content}</h3>`;
        case 4:
          return html`<h4 class=${cls}>${content}</h4>`;
        case 5:
          return html`<h5 class=${cls}>${content}</h5>`;
        case 6:
          return html`<h6 class=${cls}>${content}</h6>`;
        default:
          return html`<h1 class=${cls}>${content}</h1>`;
      }
    }
    case "paragraph": {
      const p = token as Tokens.Paragraph;
      return html`<p class="md-paragraph">${renderInlineTokens(p.tokens, options)}</p>`;
    }
    case "code": {
      const c = token as Tokens.Code;
      return html`<hu-code-block
        .code=${c.text}
        .language=${c.lang || ""}
        .onCopy=${options?.onCopyCode}
      ></hu-code-block>`;
    }
    case "blockquote": {
      const bq = token as Tokens.Blockquote;
      return html`<blockquote class="md-blockquote">
        ${bq.tokens.map((t) => renderToken(t, options))}
      </blockquote>`;
    }
    case "list": {
      const list = token as Tokens.List;
      const items = list.items.map(
        (item) =>
          html`<li class="md-list-item">${item.tokens.map((t) => renderToken(t, options))}</li>`,
      );
      if (list.ordered) {
        return html`<ol class="md-list" start=${list.start || 1}>
          ${items}
        </ol>`;
      }
      return html`<ul class="md-list">
        ${items}
      </ul>`;
    }
    case "table": {
      const table = token as Tokens.Table;
      return html`<div class="md-table-scroll">
        <table class="md-table">
          <thead>
            <tr>
              ${table.header.map(
                (cell) => html`<th class="md-th">${renderInlineTokens(cell.tokens, options)}</th>`,
              )}
            </tr>
          </thead>
          <tbody>
            ${table.rows.map(
              (row) =>
                html`<tr>
                  ${row.map(
                    (cell) =>
                      html`<td class="md-td">${renderInlineTokens(cell.tokens, options)}</td>`,
                  )}
                </tr>`,
            )}
          </tbody>
        </table>
      </div>`;
    }
    case "hr":
      return html`<hr class="md-hr" />`;
    case "html": {
      const htmlToken = token as Tokens.HTML;
      const clean = DOMPurify.sanitize(htmlToken.text, {
        ALLOWED_TAGS: [
          "strong",
          "em",
          "code",
          "a",
          "br",
          "del",
          "p",
          "div",
          "span",
          "ul",
          "ol",
          "li",
          "blockquote",
          "pre",
          "h1",
          "h2",
          "h3",
          "h4",
          "h5",
          "h6",
          "img",
          "table",
          "thead",
          "tbody",
          "tr",
          "th",
          "td",
          "hr",
        ],
        ALLOWED_ATTR: ["href", "target", "rel", "class", "src", "alt", "loading"],
      });
      return html`${unsafeHTML(clean)}`;
    }
    case "space":
      return nothing;
    case "text": {
      const textToken = token as Tokens.Text;
      if (textToken.tokens && textToken.tokens.length > 0) {
        return html`<p class="md-paragraph">${renderInlineTokens(textToken.tokens, options)}</p>`;
      }
      return html`<p class="md-paragraph">${textToken.text}</p>`;
    }
    default:
      return nothing;
  }
}

export function renderMarkdown(text: string, options?: RenderOptions): TemplateResult {
  const normalized = normalizeForParsing(text, options?.streaming ?? false);
  const tokens = marked.lexer(normalized);
  return html`<div class="md-content">${tokens.map((t) => renderToken(t, options))}</div>`;
}
