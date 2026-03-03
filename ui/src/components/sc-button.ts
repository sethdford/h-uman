import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

type ButtonVariant = "primary" | "secondary" | "destructive" | "ghost";
type ButtonSize = "sm" | "md" | "lg";

@customElement("sc-button")
export class ScButton extends LitElement {
  @property({ type: String }) variant: ButtonVariant = "secondary";
  @property({ type: String }) size: ButtonSize = "md";
  @property({ type: Boolean }) loading = false;
  @property({ type: Boolean }) disabled = false;
  @property({ type: Boolean, attribute: "icon-only" }) iconOnly = false;

  static styles = css`
    @keyframes sc-spin {
      to {
        transform: rotate(360deg);
      }
    }

    :host {
      display: inline-block;
    }

    button {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 0.375rem;
      border: none;
      outline: none;
      font-family: var(--sc-font);
      font-weight: var(--sc-weight-medium);
      cursor: pointer;
      transition:
        background-color var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out),
        transform var(--sc-duration-fast) var(--sc-ease-out);
      border-radius: var(--sc-radius);
    }

    button:active {
      transform: scale(0.98);
    }

    /* Variants */
    button.variant-primary {
      background: var(--sc-accent);
      color: white;
    }
    button.variant-primary:hover:not(:disabled) {
      background: var(--sc-accent-hover);
    }

    button.variant-secondary {
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
      border: 1px solid var(--sc-border);
    }
    button.variant-secondary:hover:not(:disabled) {
      background: var(--sc-bg-overlay);
    }

    button.variant-destructive {
      background: var(--sc-error-dim);
      color: var(--sc-error);
    }
    button.variant-destructive:hover:not(:disabled) {
      background: var(--sc-error-dim);
    }

    button.variant-ghost {
      background: transparent;
      color: var(--sc-text-muted);
    }
    button.variant-ghost:hover:not(:disabled) {
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
    }

    /* Sizes */
    button.size-sm {
      font-size: var(--sc-text-xs);
      padding: 0.375rem 0.625rem;
    }
    button.size-sm.icon-only {
      padding: 0.375rem;
    }

    button.size-md {
      font-size: var(--sc-text-sm);
      padding: 0.5rem 0.875rem;
    }
    button.size-md.icon-only {
      padding: 0.5rem;
    }

    button.size-lg {
      font-size: var(--sc-text-base);
      padding: 0.625rem 1.125rem;
    }
    button.size-lg.icon-only {
      padding: 0.625rem;
    }

    button:disabled {
      opacity: 0.5;
      pointer-events: none;
    }

    .spinner {
      width: 14px;
      height: 14px;
      border: 2px solid currentColor;
      border-right-color: transparent;
      border-radius: 50%;
      animation: sc-spin 0.6s linear infinite;
      flex-shrink: 0;
    }

    .slot {
      display: inline-flex;
      align-items: center;
      justify-content: center;
    }
  `;

  render() {
    const classes = [
      `variant-${this.variant}`,
      `size-${this.size}`,
      this.iconOnly ? "icon-only" : "",
    ]
      .filter(Boolean)
      .join(" ");

    return html`
      <button
        class=${classes}
        ?disabled=${this.disabled}
        aria-busy=${this.loading}
        aria-disabled=${this.disabled}
      >
        ${this.loading ? html`<span class="spinner" aria-hidden="true"></span>` : null}
        <span class="slot">
          <slot></slot>
        </span>
      </button>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-button": ScButton;
  }
}
