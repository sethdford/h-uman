import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("hu-thinking")
export class ScThinking extends LitElement {
  @property({ type: Boolean }) active = false;
  @property({ type: Array }) steps: string[] = [];
  @property({ type: Boolean }) expanded = false;
  @property({ type: Number }) duration = 0;

  static override styles = css`
    @keyframes hu-dot-bounce {
      0%,
      80%,
      100% {
        transform: translateY(0);
      }
      40% {
        transform: translateY(calc(-1 * var(--hu-space-xs)));
      }
    }

    :host {
      display: block;
    }

    .container {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-sm) var(--hu-space-md);
      border-radius: var(--hu-radius-lg);
      background: color-mix(
        in srgb,
        var(--hu-surface, var(--hu-bg-surface)) var(--hu-glass-subtle-bg-opacity, 4%),
        transparent
      );
      backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px))
        saturate(var(--hu-glass-subtle-saturate, 120%));
      -webkit-backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px))
        saturate(var(--hu-glass-subtle-saturate, 120%));
      border: 1px solid
        color-mix(in srgb, var(--hu-border) var(--hu-glass-subtle-border-opacity, 5%), transparent);
      cursor: pointer;
      transition: background-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .container:hover {
      background: color-mix(
        in srgb,
        var(--hu-surface, var(--hu-bg-surface)) calc(var(--hu-glass-subtle-bg-opacity, 4%) * 1.5),
        transparent
      );
    }

    .container:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .header {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }

    .dots {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
    }

    .dot {
      width: var(--hu-space-xs);
      height: var(--hu-space-xs);
      border-radius: 50%;
      background: var(--hu-info);
      animation: hu-dot-bounce var(--hu-duration-slow) ease-in-out infinite;
    }

    .dot:nth-child(2) {
      animation-delay: var(--hu-duration-normal);
    }

    .dot:nth-child(3) {
      animation-delay: var(--hu-duration-moderate);
    }

    .summary {
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }

    .steps {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
    }

    .step {
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      padding-left: var(--hu-space-md);
      position: relative;
    }

    .step::before {
      content: attr(data-num);
      position: absolute;
      left: 0;
      color: var(--hu-text-muted);
      font-weight: var(--hu-weight-medium);
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
    "hu-thinking": ScThinking;
  }
}
