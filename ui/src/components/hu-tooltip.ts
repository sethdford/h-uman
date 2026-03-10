import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

type TooltipPosition = "top" | "bottom" | "left" | "right";

let tooltipIdCounter = 0;

@customElement("hu-tooltip")
export class ScTooltip extends LitElement {
  static override styles = css`
    :host {
      display: inline-block;
      position: relative;
    }

    .wrapper {
      position: relative;
      display: inline-block;
    }

    .tip {
      position: absolute;
      white-space: nowrap;
      background: var(--hu-bg-overlay);
      color: var(--hu-text);
      font-size: var(--hu-text-xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      border-radius: var(--hu-radius-sm);
      box-shadow: var(--hu-shadow-md);
      pointer-events: none;
      opacity: 0;
      visibility: hidden;
      transform-origin: center;
      z-index: 1000;
    }

    :host(:hover) .tip,
    :host(:focus-within) .tip {
      opacity: 1;
      visibility: visible;
      animation: hu-scale-in var(--hu-duration-fast) var(--hu-spring-out);
    }

    .tip.top {
      bottom: 100%;
      left: 50%;
      transform: translateX(-50%);
      margin-bottom: var(--hu-space-xs);
    }

    .tip.bottom {
      top: 100%;
      left: 50%;
      transform: translateX(-50%);
      margin-top: var(--hu-space-xs);
    }

    .tip.left {
      right: 100%;
      top: 50%;
      transform: translateY(-50%);
      margin-right: var(--hu-space-xs);
    }

    .tip.right {
      left: 100%;
      top: 50%;
      transform: translateY(-50%);
      margin-left: var(--hu-space-xs);
    }
  `;

  @property({ type: String }) text = "";
  @property({ type: String }) position: TooltipPosition = "top";
  @state() private _tipId = `hu-tip-${tooltipIdCounter++}`;

  override render() {
    return html`
      <div class="wrapper" aria-describedby=${this._tipId}>
        <slot></slot>
        <div id=${this._tipId} class="tip ${this.position}" role="tooltip">${this.text}</div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-tooltip": ScTooltip;
  }
}
