import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

type SkeletonVariant = "line" | "card" | "circle" | "stat-card" | "channel-card" | "session-card";
type SkeletonAnimation = "shimmer" | "pulse";

@customElement("hu-skeleton")
export class ScSkeleton extends LitElement {
  static override styles = css`
    :host {
      display: inline-block;
      animation: hu-fade-in var(--hu-duration-normal) var(--hu-ease-out) backwards;
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
        105deg,
        var(--hu-bg-elevated) 25%,
        var(--hu-bg-overlay) 37%,
        var(--hu-bg-elevated) 63%
      );
      background-size: 400% 100%;
    }

    .skeleton.animation-shimmer {
      animation: hu-skel-shimmer var(--hu-duration-slow) var(--hu-ease-in-out) infinite;
    }
    @keyframes hu-skel-shimmer {
      0% {
        background-position: 100% 50%;
      }
      100% {
        background-position: 0% 50%;
      }
    }

    .skeleton.animation-pulse {
      background: var(--hu-bg-elevated);
      animation: hu-pulse var(--hu-duration-slower) infinite;
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
      border-radius: var(--hu-radius-sm);
    }
    .skeleton.card {
      border-radius: var(--hu-radius-lg);
    }
    .skeleton.circle {
      border-radius: 50%;
    }

    /* Content-aware stat card skeleton */
    .skeleton.stat-card {
      border-radius: var(--hu-radius-lg);
      position: relative;
    }
    .stat-inner {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-lg);
    }
    .stat-inner .skel-label {
      height: 0.625rem;
      width: 60%;
      background: var(--hu-bg-overlay);
      border-radius: var(--hu-radius-sm);
    }
    .stat-inner .skel-value {
      height: 1.75rem;
      width: 40%;
      background: var(--hu-bg-overlay);
      border-radius: var(--hu-radius-sm);
    }
    .stat-inner .skel-spark {
      height: 1.5rem;
      width: 5rem;
      background: var(--hu-bg-overlay);
      border-radius: var(--hu-radius-sm);
      align-self: flex-end;
      margin-top: auto;
    }

    /* Content-aware channel card skeleton */
    .skeleton.channel-card {
      border-radius: var(--hu-radius-lg);
    }
    .channel-inner {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-lg);
    }
    .channel-inner .skel-name {
      flex: 1;
      height: 1rem;
      background: var(--hu-bg-overlay);
      border-radius: var(--hu-radius-sm);
    }
    .channel-inner .skel-badge {
      height: 1.375rem;
      width: 3.5rem;
      background: var(--hu-bg-overlay);
      border-radius: var(--hu-radius-full);
    }

    /* Content-aware session card skeleton */
    .skeleton.session-card {
      border-radius: var(--hu-radius-lg);
    }
    .session-inner {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-md) var(--hu-space-lg);
    }
    .session-inner .skel-title {
      height: 0.875rem;
      width: 70%;
      background: var(--hu-bg-overlay);
      border-radius: var(--hu-radius-sm);
    }
    .session-inner .skel-meta {
      height: 0.625rem;
      width: 50%;
      background: var(--hu-bg-overlay);
      border-radius: var(--hu-radius-sm);
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
        return "var(--hu-text-base)";
      case "card":
        return "120px";
      case "circle":
        return "40px";
      case "stat-card":
        return "auto";
      case "channel-card":
        return "auto";
      case "session-card":
        return "auto";
      default:
        return "var(--hu-text-base)";
    }
  }

  override render() {
    if (this.variant === "stat-card") {
      return html`
        <div
          class="skeleton stat-card animation-${this.animation}"
          style="width: ${this.width};"
          role="status"
          aria-busy="true"
          aria-label="Loading"
        >
          <div class="stat-inner">
            <div class="skel-label"></div>
            <div class="skel-value"></div>
            <div class="skel-spark"></div>
          </div>
        </div>
      `;
    }
    if (this.variant === "channel-card") {
      return html`
        <div
          class="skeleton channel-card animation-${this.animation}"
          style="width: ${this.width};"
          role="status"
          aria-busy="true"
          aria-label="Loading"
        >
          <div class="channel-inner">
            <div class="skel-name"></div>
            <div class="skel-badge"></div>
          </div>
        </div>
      `;
    }
    if (this.variant === "session-card") {
      return html`
        <div
          class="skeleton session-card animation-${this.animation}"
          style="width: ${this.width};"
          role="status"
          aria-busy="true"
          aria-label="Loading"
        >
          <div class="session-inner">
            <div class="skel-title"></div>
            <div class="skel-meta"></div>
          </div>
        </div>
      `;
    }

    const style =
      this.variant === "circle"
        ? `width: ${this.effectiveHeight}; height: ${this.effectiveHeight};`
        : `width: ${this.width}; height: ${this.effectiveHeight};`;
    return html`
      <div
        class="skeleton ${this.variant} animation-${this.animation}"
        style="${style}"
        role="status"
        aria-busy="true"
        aria-label="Loading"
      ></div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-skeleton": ScSkeleton;
  }
}
