import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-card")
export class ScCard extends LitElement {
  @property({ type: Boolean, reflect: true }) hoverable = false;
  @property({ type: Boolean, reflect: true }) clickable = false;
  @property({ type: Boolean }) accent = false;
  @property({ type: Boolean }) elevated = false;
  @property({ type: Boolean }) glass = true;
  @property({ type: Boolean }) solid = false;

  static override styles = css`
    :host {
      display: block;
    }

    .card {
      position: relative;
      background: var(--sc-bg-surface);
      background-image: var(--sc-surface-gradient);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-xl);
      padding: var(--sc-space-xl);
      box-shadow:
        var(--sc-shadow-card),
        inset 0 1px 0 color-mix(in srgb, var(--sc-color-white) 90%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--sc-text) 4%, transparent);
      overflow: hidden;
    }

    /* Gradient border glow — bright top edge fading to transparent bottom (Apple Liquid Glass) */
    .card::before {
      content: "";
      position: absolute;
      inset: 0;
      border-radius: inherit;
      background: linear-gradient(
        180deg,
        color-mix(in srgb, var(--sc-color-white) 70%, transparent),
        color-mix(in srgb, var(--sc-color-white) 10%, transparent) 30%,
        transparent 60%
      );
      mask:
        linear-gradient(white 0 0) content-box,
        linear-gradient(white 0 0);
      mask-composite: exclude;
      -webkit-mask:
        linear-gradient(white 0 0) content-box,
        linear-gradient(white 0 0);
      -webkit-mask-composite: xor;
      padding: 1px;
      pointer-events: none;
      z-index: 1;
    }

    /* Ambient teal surface glow from top */
    .card::after {
      content: "";
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 6.25rem;
      background: radial-gradient(
        ellipse 90% 70% at 50% -20%,
        color-mix(in srgb, var(--sc-accent) 6%, transparent),
        transparent
      );
      border-radius: inherit;
      pointer-events: none;
      z-index: 0;
    }

    .card > ::slotted(*),
    .card > * {
      position: relative;
      z-index: 2;
    }

    .card.elevated {
      box-shadow:
        var(--sc-shadow-md),
        inset 0 1px 0 color-mix(in srgb, var(--sc-color-white) 90%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--sc-text) 4%, transparent);
    }

    /* Accent top-band — teal gradient bar + tinted wash below */
    .card.accent {
      border-top: none;
      padding-top: calc(var(--sc-space-xl) + var(--sc-space-xs));
    }
    .card.accent::after {
      height: var(--sc-space-xs);
      top: 0;
      background: linear-gradient(
        90deg,
        var(--sc-accent),
        color-mix(in srgb, var(--sc-accent) 40%, transparent)
      );
      border-radius: var(--sc-radius-xl) var(--sc-radius-xl) 0 0;
    }
    .card.accent::before {
      background: linear-gradient(
        180deg,
        color-mix(in srgb, var(--sc-accent) 6%, transparent),
        transparent 40%
      );
      mask: none;
      -webkit-mask: none;
      padding: 0;
    }

    /* Glass variant — Apple Liquid Glass with specular highlight */
    .card.glass {
      background: color-mix(in srgb, var(--sc-bg-surface) 65%, transparent);
      backdrop-filter: blur(var(--sc-blur-lg)) saturate(var(--sc-glass-standard-saturate));
      -webkit-backdrop-filter: blur(var(--sc-blur-lg)) saturate(var(--sc-glass-standard-saturate));
      border: 1px solid color-mix(in srgb, var(--sc-border-subtle) 40%, transparent);
      box-shadow:
        var(--sc-shadow-card),
        inset 0 1px 0 color-mix(in srgb, white 60%, transparent),
        inset 0 -1px 0 color-mix(in srgb, var(--sc-text) 3%, transparent);
    }
    .card.glass::before {
      background: radial-gradient(
        ellipse 60% 40% at 25% 0%,
        color-mix(in srgb, var(--sc-color-white) 15%, transparent),
        transparent 70%
      );
      mask: none;
      -webkit-mask: none;
      padding: 0;
    }

    .card.hoverable,
    .card.clickable {
      cursor: pointer;
      will-change: transform, box-shadow;
    }
    :host([hoverable]),
    :host([clickable]) {
      transition:
        transform var(--sc-duration-normal) var(--sc-ease-spring),
        box-shadow var(--sc-duration-normal) var(--sc-ease-out);
    }
    :host([hoverable]:hover),
    :host([clickable]:hover) {
      transform: translateY(var(--sc-physics-card-hover-translateY, -2px));
      box-shadow: var(--sc-shadow-lg);
    }
    :host([hoverable]:active),
    :host([clickable]:active) {
      transform: translateY(0) scale(0.99);
    }
    .card.clickable:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    @media (prefers-reduced-motion: reduce) {
      :host([hoverable]),
      :host([clickable]) {
        transition: none !important;
        animation: none !important;
      }
    }
  `;

  private _onKeyDown(e: KeyboardEvent): void {
    if (!this.clickable || (e.key !== "Enter" && e.key !== " ")) return;
    e.preventDefault();
    this.dispatchEvent(new MouseEvent("click", { bubbles: true, composed: true }));
  }

  render() {
    const classes = [
      "card",
      this.hoverable ? "hoverable" : "",
      this.clickable ? "clickable" : "",
      this.accent ? "accent" : "",
      this.elevated ? "elevated" : "",
      !this.solid ? "glass" : "",
    ]
      .filter(Boolean)
      .join(" ");

    return html`
      <div
        class=${classes}
        role=${this.clickable ? "button" : undefined}
        tabindex=${this.clickable ? 0 : undefined}
        @keydown=${this._onKeyDown}
      >
        <slot></slot>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-card": ScCard;
  }
}
