import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

type SkeletonVariant = "line" | "card" | "circle";
type SkeletonAnimation = "shimmer" | "pulse";

@customElement("sc-skeleton")
export class ScSkeleton extends LitElement {
  static override styles = css`
    :host {
      display: inline-block;
      animation: sc-fade-in var(--sc-duration-normal) var(--sc-ease-out) backwards;
    }
    :host(:nth-child(1)) {
      animation-delay: 0ms;
    }
    :host(:nth-child(2)) {
      animation-delay: 40ms;
    }
    :host(:nth-child(3)) {
      animation-delay: 80ms;
    }
    :host(:nth-child(4)) {
      animation-delay: 120ms;
    }
    :host(:nth-child(5)) {
      animation-delay: 160ms;
    }
    :host(:nth-child(6)) {
      animation-delay: 200ms;
    }
    :host(:nth-child(7)) {
      animation-delay: 240ms;
    }
    :host(:nth-child(8)) {
      animation-delay: 280ms;
    }
    :host(:nth-child(9)) {
      animation-delay: 320ms;
    }
    :host(:nth-child(10)) {
      animation-delay: 360ms;
    }

    .skeleton {
      background: linear-gradient(
        90deg,
        var(--sc-bg-elevated) 25%,
        var(--sc-bg-overlay) 50%,
        var(--sc-bg-elevated) 75%
      );
      background-size: 200% 100%;
    }

    .skeleton.animation-shimmer {
      animation: sc-shimmer var(--sc-duration-slower) infinite;
    }

    .skeleton.animation-pulse {
      background: var(--sc-bg-elevated);
      animation: sc-pulse var(--sc-duration-slower) infinite;
    }

    @media (prefers-reduced-motion: reduce) {
      :host {
        animation: none;
      }
      .skeleton.animation-shimmer,
      .skeleton.animation-pulse {
        animation: none;
      }
    }

    .skeleton.line {
      border-radius: var(--sc-radius-sm);
    }

    .skeleton.card {
      border-radius: var(--sc-radius-lg);
    }

    .skeleton.circle {
      border-radius: 50%;
    }
  `;

  @property({ type: String }) variant: SkeletonVariant = "line";

  @property({ type: String }) animation: SkeletonAnimation = "shimmer";
  @property({ type: String }) width = "100%";
  @property({ type: String }) height = "";

  private get effectiveHeight(): string {
    if (this.height) return this.height;
    switch (this.variant) {
      case "line":
        return "var(--sc-text-base)";
      case "card":
        return "120px";
      case "circle":
        return "40px";
      default:
        return "var(--sc-text-base)";
    }
  }

  override render() {
    const style =
      this.variant === "circle"
        ? `width: ${this.effectiveHeight}; height: ${this.effectiveHeight};`
        : `width: ${this.width}; height: ${this.effectiveHeight};`;
    return html`
      <div class="skeleton ${this.variant} animation-${this.animation}" style="${style}"></div>
    `;
  }
}
