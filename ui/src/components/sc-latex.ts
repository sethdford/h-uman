import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { unsafeHTML } from "lit/directives/unsafe-html.js";

@customElement("sc-latex")
export class ScLatex extends LitElement {
  @property({ type: String }) latex = "";
  @property({ type: Boolean, reflect: true }) display = false;

  @property({ type: String }) private _rendered = "";
  @property({ type: Boolean }) private _loaded = false;

  static override styles = css`
    :host {
      display: inline;
      font-family: var(--sc-font-mono);
    }
    :host([display]) {
      display: block;
      margin: var(--sc-space-sm) 0;
      overflow-x: auto;
    }
    .katex {
      font-size: 1em;
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    if (this.latex && !this._loaded) {
      import("katex")
        .then((katex) => {
          try {
            this._rendered = katex.default.renderToString(this.latex, {
              displayMode: this.display,
              throwOnError: false,
            });
          } catch {
            this._rendered = this.latex;
          }
          this._loaded = true;
          this.requestUpdate();
        })
        .catch(() => {
          this._rendered = this.latex;
          this._loaded = true;
          this.requestUpdate();
        });
    }
  }

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("latex") && this.latex && this._loaded) {
      import("katex")
        .then((katex) => {
          try {
            this._rendered = katex.default.renderToString(this.latex, {
              displayMode: this.display,
              throwOnError: false,
            });
          } catch {
            this._rendered = this.latex;
          }
          this.requestUpdate();
        })
        .catch(() => {
          this._rendered = this.latex;
          this.requestUpdate();
        });
    }
  }

  override render() {
    if (this._rendered) {
      return html`<span class="katex">${unsafeHTML(this._rendered)}</span>`;
    }
    return html`<span class="latex-raw">${this.latex}</span>`;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-latex": ScLatex;
  }
}
