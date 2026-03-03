import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

type SkeletonVariant = "line" | "card" | "circle";

@customElement("sc-skeleton")
export class ScSkeleton extends LitElement {
  static override styles = css`
    :host {
      display: inline-block;
    }

    .skeleton {
      background: linear-gradient(
        90deg,
        var(--sc-bg-elevated) 25%,
        var(--sc-bg-overlay) 50%,
        var(--sc-bg-elevated) 75%
      );
      background-size: 200% 100%;
      animation: sc-shimmer 1.5s infinite;
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
  @property({ type: String }) width = "100%";
  @property({ type: String }) height = "";

  private get effectiveHeight(): string {
    if (this.height) return this.height;
    switch (this.variant) {
      case "line":
        return "0.875rem";
      case "card":
        return "120px";
      case "circle":
        return "40px";
      default:
        return "0.875rem";
    }
  }

  override render() {
    const style =
      this.variant === "circle"
        ? `width: ${this.effectiveHeight}; height: ${this.effectiveHeight};`
        : `width: ${this.width}; height: ${this.effectiveHeight};`;
    return html` <div class="skeleton ${this.variant}" style="${style}"></div> `;
  }
}
