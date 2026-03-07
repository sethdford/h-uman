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

  static override styles = css`
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
        box-shadow var(--sc-duration-moderate) var(--sc-emphasize),
        transform var(--sc-duration-fast) var(--sc-ease-spring, var(--sc-ease-out));
      border-radius: var(--sc-radius);
    }

    button:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    /* Primary — Fidelity-style convex pillow with specular highlights */
    button.variant-primary {
      background: var(--sc-accent);
      background-image: var(--sc-button-gradient-primary);
      color: var(--sc-on-accent, #ffffff);
      text-shadow: 0 1px 1px rgba(0, 0, 0, 0.2);
      box-shadow:
        var(--sc-shadow-sm),
        inset 0 1px 0 rgba(255, 255, 255, 0.3),
        inset 0 -1px 0 rgba(0, 0, 0, 0.15);
    }
    button.variant-primary:hover:not(:disabled) {
      background: var(--sc-accent-hover);
      background-image: var(--sc-button-gradient-primary);
      transform: translateY(var(--sc-physics-card-hover-translateY, -2px)) scale(1.02);
      box-shadow:
        0 4px 16px rgba(90, 154, 48, 0.25),
        0 2px 6px rgba(6, 18, 36, 0.1),
        inset 0 1px 0 rgba(255, 255, 255, 0.35),
        inset 0 -1px 0 rgba(0, 0, 0, 0.15);
    }
    button.variant-primary:active:not(:disabled) {
      transform: translateY(0) scale(0.96);
      box-shadow:
        var(--sc-shadow-xs),
        inset 0 1px 0 rgba(255, 255, 255, 0.2),
        inset 0 -1px 0 rgba(0, 0, 0, 0.1);
      transition-duration: 80ms;
    }

    /* Secondary — subtle gradient with inner depth */
    button.variant-secondary {
      background: var(--sc-bg-elevated);
      background-image: linear-gradient(180deg, rgba(255, 255, 255, 0.6), rgba(0, 0, 0, 0.02));
      color: var(--sc-text);
      border: 1px solid var(--sc-border);
      box-shadow:
        var(--sc-shadow-xs),
        inset 0 1px 0 rgba(255, 255, 255, 0.8),
        inset 0 -1px 0 rgba(6, 18, 36, 0.04);
    }
    button.variant-secondary:hover:not(:disabled) {
      background: var(--sc-bg-overlay);
      background-image: linear-gradient(180deg, rgba(255, 255, 255, 0.7), rgba(0, 0, 0, 0.02));
      transform: translateY(var(--sc-physics-card-hover-translateY, -2px)) scale(1.02);
      box-shadow:
        var(--sc-shadow-sm),
        inset 0 1px 0 rgba(255, 255, 255, 0.9),
        inset 0 -1px 0 rgba(6, 18, 36, 0.04);
    }
    button.variant-secondary:active:not(:disabled) {
      transform: translateY(0) scale(0.96);
      box-shadow:
        inset 0 1px 2px rgba(6, 18, 36, 0.06),
        inset 0 -1px 0 rgba(6, 18, 36, 0.04);
      transition-duration: 80ms;
    }

    button.variant-destructive {
      background: var(--sc-error-dim);
      color: var(--sc-error);
      box-shadow:
        var(--sc-shadow-xs),
        inset 0 1px 0 rgba(255, 255, 255, 0.3),
        inset 0 -1px 0 rgba(0, 0, 0, 0.08);
    }
    button.variant-destructive:hover:not(:disabled) {
      transform: translateY(var(--sc-physics-card-hover-translateY, -2px)) scale(1.02);
      box-shadow:
        0 4px 16px rgba(249, 112, 102, 0.2),
        0 2px 6px rgba(6, 18, 36, 0.08),
        inset 0 1px 0 rgba(255, 255, 255, 0.3),
        inset 0 -1px 0 rgba(0, 0, 0, 0.08);
    }
    button.variant-destructive:active:not(:disabled) {
      transform: translateY(0) scale(0.96);
      transition-duration: 80ms;
    }

    button.variant-ghost {
      background: transparent;
      color: var(--sc-text-muted);
    }
    button.variant-ghost:hover:not(:disabled) {
      background: var(--sc-hover-overlay);
      color: var(--sc-text);
      transform: translateY(var(--sc-physics-card-hover-translateY, -2px)) scale(1.02);
    }
    button.variant-ghost:active:not(:disabled) {
      transform: translateY(0) scale(0.96);
      background: var(--sc-accent-subtle);
      transition-duration: 80ms;
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

    @media (prefers-reduced-motion: reduce) {
      button {
        transition: none !important;
      }
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
