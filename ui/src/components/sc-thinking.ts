import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-thinking")
export class ScThinking extends LitElement {
  @property({ type: Boolean }) active = false;
  @property({ type: Array }) steps: string[] = [];
  @property({ type: Boolean }) expanded = false;
  @property({ type: Number }) duration = 0;

  static override styles = css`
    @keyframes sc-dot-bounce {
      0%,
      80%,
      100% {
        transform: translateY(0);
      }
      40% {
        transform: translateY(-6px);
      }
    }

    :host {
      display: block;
    }

    .container {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      border-radius: var(--sc-radius-lg);
      background: color-mix(
        in srgb,
        var(--sc-surface, var(--sc-bg-surface)) var(--sc-glass-subtle-bg-opacity, 4%),
        transparent
      );
      backdrop-filter: blur(var(--sc-glass-subtle-blur, 12px))
        saturate(var(--sc-glass-subtle-saturate, 120%));
      -webkit-backdrop-filter: blur(var(--sc-glass-subtle-blur, 12px))
        saturate(var(--sc-glass-subtle-saturate, 120%));
      border: 1px solid
        color-mix(in srgb, var(--sc-border) var(--sc-glass-subtle-border-opacity, 5%), transparent);
      cursor: pointer;
      transition: background-color var(--sc-duration-fast) var(--sc-ease-out);
    }

    .container:hover {
      background: color-mix(
        in srgb,
        var(--sc-surface, var(--sc-bg-surface)) calc(var(--sc-glass-subtle-bg-opacity, 4%) * 1.5),
        transparent
      );
    }

    .container:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }

    .dots {
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
    }

    .dot {
      width: 6px;
      height: 6px;
      border-radius: 50%;
      background: var(--sc-info);
      animation: sc-dot-bounce var(--sc-duration-slow) ease-in-out infinite;
    }

    .dot:nth-child(2) {
      animation-delay: 150ms;
    }

    .dot:nth-child(3) {
      animation-delay: 300ms;
    }

    .summary {
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
    }

    .steps {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
    }

    .step {
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      padding-left: var(--sc-space-md);
      position: relative;
    }

    .step::before {
      content: attr(data-num);
      position: absolute;
      left: 0;
      color: var(--sc-text-muted);
      font-weight: var(--sc-weight-medium);
    }

    @media (prefers-reduced-motion: reduce) {
      .dot {
        animation: none;
      }
    }
  `;

  private _toggle() {
    this.expanded = !this.expanded;
  }

  private _onKeyDown(e: KeyboardEvent) {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      this._toggle();
    }
  }

  override render() {
    return html`
      <div
        class="container"
        role="button"
        tabindex="0"
        aria-expanded=${this.expanded}
        aria-label=${this.active ? "Thinking" : "Thought summary"}
        @click=${this._toggle}
        @keydown=${this._onKeyDown}
      >
        ${this.active
          ? html`
              <div class="header">
                <span>Thinking</span>
                <span class="dots"
                  ><span class="dot"></span><span class="dot"></span><span class="dot"></span
                ></span>
              </div>
            `
          : this.expanded
            ? html`
                <div class="summary">Thought for ${this.duration.toFixed(1)}s</div>
                ${this.steps.length > 0
                  ? html`
                      <ol class="steps">
                        ${this.steps.map(
                          (s, i) => html`<li class="step" data-num="${i + 1}. ">${s}</li>`,
                        )}
                      </ol>
                    `
                  : null}
              `
            : html` <div class="summary">Thought for ${this.duration.toFixed(1)}s</div> `}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-thinking": ScThinking;
  }
}
