import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { hapticFeedback } from "../utils/gesture.js";

type ButtonVariant = "primary" | "secondary" | "destructive" | "ghost";
type ButtonSize = "sm" | "md" | "lg";

@customElement("hu-button")
export class ScButton extends LitElement {
  @property({ type: String }) variant: ButtonVariant = "secondary";
  @property({ type: String }) size: ButtonSize = "md";
  @property({ type: Boolean }) loading = false;
  @property({ type: Boolean }) disabled = false;
  @property({ type: Boolean, attribute: "icon-only" }) iconOnly = false;
  @property({ type: String, attribute: "aria-label" }) ariaLabelAttr = "";

  static override styles = css`
    @keyframes hu-spin {
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
      gap: var(--hu-space-xs);
      box-sizing: border-box;
      min-height: var(--hu-button-min-height, 2.75rem);
      border: none;
      outline: none;
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-medium);
      cursor: pointer;
      transition:
        background-color var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-moderate) var(--hu-emphasize),
        transform var(--hu-duration-fast) var(--hu-ease-spring, var(--hu-ease-out));
      border-radius: var(--hu-radius);
    }

    button.icon-only {
      min-width: var(--hu-button-min-height);
    }

    button:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    /* Primary — Human-style convex pillow with specular highlights.
       --hu-on-accent on --hu-accent is 1.9-3.5:1 under the gradient in both
       themes. This pair holds 6.7:1 dark and 4.6:1 light across the text band
       (30-70% of the height), where the light theme's white top veil is thinnest. */
    button.variant-primary {
      background: var(--hu-accent-text);
      background-image: var(--hu-button-gradient-primary);
      color: var(--hu-bg-surface);
      text-shadow: 0 1px 1px color-mix(in srgb, var(--hu-text) 20%, transparent);
      box-shadow:
        var(--hu-shadow-sm),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 30%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-color-black) 15%, transparent);
    }
    button.variant-primary:hover:not(:disabled) {
      background: var(--hu-accent-text);
      background-image:
        linear-gradient(var(--hu-hover-overlay), var(--hu-hover-overlay)),
        var(--hu-button-gradient-primary);
      transform: translateY(-1px);
      box-shadow:
        var(--hu-shadow-md),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 35%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-color-black) 15%, transparent);
    }
    button.variant-primary:active:not(:disabled) {
      transform: translateY(0) scale(0.98);
      box-shadow:
        var(--hu-shadow-xs),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 20%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-color-black) 10%, transparent);
      transition-duration: var(--hu-duration-fast);
    }

    /* prefers-contrast: more forces a black surface but keeps --hu-accent-text
       dark (3.3:1); its accent/on-accent pair is the one tuned for that mode. */
    @media (prefers-contrast: more) {
      button.variant-primary {
        background-color: var(--hu-accent);
        color: var(--hu-on-accent);
      }
      button.variant-primary:hover:not(:disabled) {
        background-color: var(--hu-accent-hover);
      }
    }

    /* Secondary — subtle gradient with inner depth */
    button.variant-secondary {
      background: var(--hu-bg-elevated);
      background-image: var(--hu-surface-gradient);
      color: var(--hu-text);
      border: 1px solid var(--hu-border);
      box-shadow:
        var(--hu-shadow-xs),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 80%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-text) 4%, transparent);
    }
    button.variant-secondary:hover:not(:disabled) {
      background: var(--hu-bg-overlay);
      background-image: var(--hu-surface-gradient);
      transform: translateY(-1px);
      box-shadow:
        var(--hu-shadow-sm),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 90%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-text) 4%, transparent);
    }
    button.variant-secondary:active:not(:disabled) {
      background: var(--hu-pressed-overlay);
      transform: translateY(0) scale(0.98);
      box-shadow:
        inset 0 1px 2px color-mix(in srgb, var(--hu-text) 6%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-text) 4%, transparent);
      transition-duration: var(--hu-duration-fast);
    }

    button.variant-destructive {
      background: var(--hu-error-dim);
      color: var(--hu-error);
      box-shadow:
        var(--hu-shadow-xs),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 30%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-color-black) 8%, transparent);
    }
    button.variant-destructive:hover:not(:disabled) {
      transform: translateY(-1px);
      box-shadow:
        0 4px 16px color-mix(in srgb, var(--hu-error) 20%, transparent),
        0 2px 6px color-mix(in srgb, var(--hu-text) 8%, transparent),
        inset 0 1px 0 color-mix(in srgb, var(--hu-color-white) 30%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--hu-color-black) 8%, transparent);
    }
    button.variant-destructive:active:not(:disabled) {
      transform: translateY(0) scale(0.98);
      transition-duration: var(--hu-duration-fast);
    }

    button.variant-ghost {
      background: transparent;
      color: var(--hu-text-muted);
    }
    button.variant-ghost:hover:not(:disabled) {
      background: var(--hu-hover-overlay);
      color: var(--hu-text);
    }
    button.variant-ghost:active:not(:disabled) {
      transform: scale(0.98);
      background: var(--hu-accent-subtle);
      transition-duration: var(--hu-duration-fast);
    }

    /* Sizes */
    button.size-sm {
      font-size: var(--hu-text-xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
    }
    button.size-sm.icon-only {
      padding: var(--hu-space-xs);
    }

    button.size-md {
      font-size: var(--hu-text-sm);
      padding: var(--hu-space-sm) var(--hu-space-md);
    }
    button.size-md.icon-only {
      padding: var(--hu-space-sm);
    }

    button.size-lg {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-lg);
    }
    button.size-lg.icon-only {
      padding: var(--hu-space-sm);
    }

    button:disabled {
      opacity: var(--hu-opacity-disabled, 0.5);
      pointer-events: none;
    }

    .spinner {
      width: var(--hu-text-base);
      height: var(--hu-text-base);
      border: 2px solid currentColor;
      border-right-color: transparent;
      border-radius: 50%;
      animation: hu-spin var(--hu-duration-slow) linear infinite;
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
      .spinner {
        animation: none !important;
      }
    }

    @media (prefers-contrast: more) {
      button {
        border: 2px solid currentColor;
        box-shadow: none;
      }
    }

    :host([full-width]) button {
      width: 100%;
      justify-content: center;
    }
  `;

  private _onPointerDown = (): void => {
    if (!this.disabled && !this.loading) hapticFeedback("light");
  };

  override connectedCallback(): void {
    super.connectedCallback();
    this.addEventListener("pointerdown", this._onPointerDown);
    if (this.iconOnly && !this.ariaLabelAttr) {
      console.warn(
        `<hu-button icon-only> missing aria-label — screen readers cannot name this control`,
      );
    }
  }

  override disconnectedCallback(): void {
    this.removeEventListener("pointerdown", this._onPointerDown);
    super.disconnectedCallback();
  }

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
        ?disabled=${this.disabled || this.loading}
        aria-busy=${this.loading}
        aria-disabled=${this.disabled || this.loading}
        aria-label=${this.ariaLabelAttr || nothing}
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
    "hu-button": ScButton;
  }
}
