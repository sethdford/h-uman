import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

type TooltipPosition = "top" | "bottom" | "left" | "right";

let tooltipIdCounter = 0;

@customElement("sc-tooltip")
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
      background: var(--sc-bg-overlay);
      color: var(--sc-text);
      font-size: var(--sc-text-xs);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      border-radius: var(--sc-radius-sm);
      box-shadow: var(--sc-shadow-md);
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
      animation: sc-scale-in var(--sc-duration-fast) var(--sc-spring-out);
    }

    .tip.top {
      bottom: 100%;
      left: 50%;
      transform: translateX(-50%);
      margin-bottom: var(--sc-space-xs);
    }

    .tip.bottom {
      top: 100%;
      left: 50%;
      transform: translateX(-50%);
      margin-top: var(--sc-space-xs);
    }

    .tip.left {
      right: 100%;
      top: 50%;
      transform: translateY(-50%);
      margin-right: var(--sc-space-xs);
    }

    .tip.right {
      left: 100%;
      top: 50%;
      transform: translateY(-50%);
      margin-left: var(--sc-space-xs);
    }
  `;

  @property({ type: String }) text = "";
  @property({ type: String }) position: TooltipPosition = "top";
  @state() private _tipId = `sc-tip-${tooltipIdCounter++}`;

  override render() {
    return html`
      <div class="wrapper" aria-describedby=${this._tipId}>
        <slot></slot>
        <div id=${this._tipId} class="tip ${this.position}" role="tooltip">${this.text}</div>
      </div>
    `;
  }
}
