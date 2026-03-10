import { describe, it, expect } from "vitest";
import { render } from "lit";
import { renderMarkdown } from "./markdown.js";

/** Render a TemplateResult to a container and return the container's innerHTML. */
function renderToHtml(template: ReturnType<typeof renderMarkdown>): string {
  const container = document.createElement("div");
  document.body.appendChild(container);
  render(template, container);
  const html = container.innerHTML;
  container.remove();
  return html;
}

describe("renderMarkdown", () => {
  it("renders plain text", () => {
    const result = renderMarkdown("Hello world");
    const out = renderToHtml(result);
    expect(out).toContain("Hello world");
    expect(out).toContain("md-content");
  });

  it("renders heading", () => {
    const result = renderMarkdown("# Main Title");
    const out = renderToHtml(result);
    expect(out).toContain("h1");
    expect(out).toContain("Main Title");
    expect(out).toContain("md-heading");
  });

  it("renders code block as hu-code-block element", () => {
    const result = renderMarkdown("```js\nconst x = 1;\n```");
    const out = renderToHtml(result);
    expect(out).toContain("hu-code-block");
  });

  it("renders unordered list", () => {
    const result = renderMarkdown("- Item A\n- Item B");
    const out = renderToHtml(result);
    expect(out).toContain("ul");
    expect(out).toContain("md-list");
    expect(out).toContain("Item A");
    expect(out).toContain("Item B");
  });

  it("renders ordered list", () => {
    const result = renderMarkdown("1. First\n2. Second");
    const out = renderToHtml(result);
    expect(out).toContain("ol");
    expect(out).toContain("md-list");
    expect(out).toContain("First");
    expect(out).toContain("Second");
  });

  it("renders blockquote", () => {
    const result = renderMarkdown("> A quoted line");
    const out = renderToHtml(result);
    expect(out).toContain("blockquote");
    expect(out).toContain("md-blockquote");
    expect(out).toContain("A quoted line");
  });

  it("renders table", () => {
    const result = renderMarkdown("| A | B |\n|---|---|\n| 1 | 2 |");
    const out = renderToHtml(result);
    expect(out).toContain("table");
    expect(out).toContain("md-table");
    expect(out).toContain("thead");
    expect(out).toContain("tbody");
  });

  it("renders horizontal rule", () => {
    const result = renderMarkdown("---");
    const out = renderToHtml(result);
    expect(out).toContain("hr");
    expect(out).toContain("md-hr");
  });

  it("renders link with target=_blank", () => {
    const result = renderMarkdown("[link](https://example.com)");
    const out = renderToHtml(result);
    expect(out).toContain('href="https://example.com"');
    expect(out).toContain('target="_blank"');
    expect(out).toContain("link");
  });

  it("handles streaming with unclosed code fence", () => {
    const result = renderMarkdown("```js\nconst x = 1;\n", { streaming: true });
    const out = renderToHtml(result);
    expect(out).toContain("hu-code-block");
  });

  it("renders inline bold, italic, code", () => {
    const result = renderMarkdown("**bold** _italic_ `code`");
    const out = renderToHtml(result);
    expect(out).toContain("<strong>");
    expect(out).toContain("bold");
    expect(out).toContain("<em>");
    expect(out).toContain("italic");
    expect(out).toContain('class="inline"');
  });
});
