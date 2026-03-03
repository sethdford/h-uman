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
      gap: var(--sc-space-xs);
      border: none;
      outline: none;
      font-family: var(--sc-font);
      font-weight: var(--sc-weight-medium);
      cursor: pointer;
      transition:
        background-color var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-normal) var(--sc-ease-out),
        transform var(--sc-duration-normal) var(--sc-spring-out);
      border-radius: var(--sc-radius);
    }

    button:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    /* Variants */
    button.variant-primary {
      background: var(--sc-accent);
      background-image: var(--sc-button-gradient-primary);
      color: var(--sc-on-accent, #ffffff);
      text-shadow: 0 1px 1px rgba(0, 0, 0, 0.15);
      box-shadow: var(--sc-shadow-sm);
    }
    button.variant-primary:hover:not(:disabled) {
      background: var(--sc-accent-hover);
      background-image: var(--sc-button-gradient-primary);
      box-shadow: var(--sc-shadow-md);
      transform: translateY(-1px);
    }
    button.variant-primary:active:not(:disabled) {
      transform: scale(0.97);
      box-shadow: var(--sc-shadow-xs);
      transition-duration: 50ms;
    }

    button.variant-secondary {
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
      border: 1px solid var(--sc-border);
      box-shadow: var(--sc-shadow-xs);
    }
    button.variant-secondary:hover:not(:disabled) {
      background: var(--sc-bg-overlay);
      box-shadow: var(--sc-shadow-sm);
      transform: translateY(-1px);
    }
    button.variant-secondary:active:not(:disabled) {
      transform: scale(0.97);
      box-shadow: none;
      transition-duration: 50ms;
    }

    button.variant-destructive {
      background: var(--sc-error-dim);
      color: var(--sc-error);
      box-shadow: var(--sc-shadow-xs);
    }
    button.variant-destructive:hover:not(:disabled) {
      box-shadow: var(--sc-shadow-sm);
      transform: translateY(-1px);
    }
    button.variant-destructive:active:not(:disabled) {
      transform: scale(0.97);
      transition-duration: 50ms;
    }

    button.variant-ghost {
      background: transparent;
      color: var(--sc-text-muted);
    }
    button.variant-ghost:hover:not(:disabled) {
      background: var(--sc-hover-overlay);
      color: var(--sc-text);
    }
    button.variant-ghost:active:not(:disabled) {
      transform: scale(0.97);
      background: var(--sc-accent-subtle);
      transition-duration: 50ms;
    }

    /* Sizes */
    button.size-sm {
      font-size: var(--sc-text-xs);
      padding: var(--sc-space-xs) var(--sc-space-sm);
    }
    button.size-sm.icon-only {
      padding: var(--sc-space-xs);
    }

    button.size-md {
      font-size: var(--sc-text-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
    }
    button.size-md.icon-only {
      padding: var(--sc-space-sm);
    }

    button.size-lg {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-sm) var(--sc-space-lg);
    }
    button.size-lg.icon-only {
      padding: var(--sc-space-sm);
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
      animation: sc-spin var(--sc-duration-slow) linear infinite;
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
