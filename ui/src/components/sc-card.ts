import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-card")
export class ScCard extends LitElement {
  @property({ type: Boolean }) hoverable = false;
  @property({ type: Boolean }) clickable = false;
  @property({ type: Boolean }) accent = false;
  @property({ type: Boolean }) elevated = false;
  @property({ type: Boolean }) glass = false;

  static override styles = css`
    :host {
      display: block;
    }

    .card {
      position: relative;
      background: var(--sc-bg-surface);
      background-image: var(--sc-surface-gradient);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-xl, 16px);
      padding: var(--sc-space-xl);
      box-shadow:
        var(--sc-shadow-card),
        inset 0 1px 0 rgba(255, 255, 255, 0.9),
        inset 0 -1px 0 rgba(6, 18, 36, 0.04);
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
        rgba(255, 255, 255, 0.7),
        rgba(255, 255, 255, 0.1) 30%,
        transparent 60%
      );
      mask:
        linear-gradient(#fff 0 0) content-box,
        linear-gradient(#fff 0 0);
      mask-composite: exclude;
      -webkit-mask:
        linear-gradient(#fff 0 0) content-box,
        linear-gradient(#fff 0 0);
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
      height: 100px;
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
        inset 0 1px 0 rgba(255, 255, 255, 0.9),
        inset 0 -1px 0 rgba(6, 18, 36, 0.04);
    }

    /* Accent top-band — teal gradient bar + tinted wash below */
    .card.accent {
      border-top: none;
      padding-top: calc(var(--sc-space-xl) + 4px);
    }
    .card.accent::after {
      height: 4px;
      top: 0;
      background: linear-gradient(
        90deg,
        var(--sc-accent),
        color-mix(in srgb, var(--sc-accent) 40%, transparent)
      );
      border-radius: var(--sc-radius-xl, 16px) var(--sc-radius-xl, 16px) 0 0;
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
      backdrop-filter: blur(24px) saturate(180%);
      -webkit-backdrop-filter: blur(24px) saturate(180%);
      border: 1px solid color-mix(in srgb, var(--sc-border-subtle) 40%, transparent);
      box-shadow:
        var(--sc-shadow-card),
        inset 0 1px 0 rgba(255, 255, 255, 0.6),
        inset 0 -1px 0 rgba(6, 18, 36, 0.03);
    }
    .card.glass::before {
      background: radial-gradient(
        ellipse 60% 40% at 25% 0%,
        rgba(255, 255, 255, 0.15),
        transparent 70%
      );
      mask: none;
      -webkit-mask: none;
      padding: 0;
    }

    .card.hoverable,
    .card.clickable {
      cursor: pointer;
      transition:
        transform var(--sc-duration-moderate, 300ms)
          var(--sc-emphasize-overshoot, cubic-bezier(0.2, 0, 0, 1.2)),
        box-shadow var(--sc-duration-moderate, 300ms)
          var(--sc-emphasize, cubic-bezier(0.2, 0, 0, 1)),
        border-color var(--sc-duration-normal, 200ms) ease;
      will-change: transform, box-shadow;
    }
    .card.hoverable:hover,
    .card.clickable:hover {
      transform: translateY(-4px) scale(1.005);
      box-shadow:
        var(--sc-shadow-lg),
        inset 0 1px 0 rgba(255, 255, 255, 0.9),
        inset 0 -1px 0 rgba(6, 18, 36, 0.04);
      border-color: color-mix(in srgb, var(--sc-accent) 20%, var(--sc-border-subtle));
    }
    .card.hoverable:active,
    .card.clickable:active {
      transform: translateY(0px) scaleX(1.003) scaleY(0.995);
      box-shadow:
        var(--sc-shadow-sm),
        inset 0 1px 0 rgba(255, 255, 255, 0.9),
        inset 0 -1px 0 rgba(6, 18, 36, 0.06);
      transition-duration: 80ms;
    }
    .card.clickable:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: var(--sc-focus-ring-offset);
    }

    @media (prefers-reduced-motion: reduce) {
      .card.hoverable,
      .card.clickable {
        transition: none;
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
      this.glass ? "glass" : "",
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
