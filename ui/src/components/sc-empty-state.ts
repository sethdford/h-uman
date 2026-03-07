import { LitElement, html, css, nothing, type TemplateResult } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-empty-state")
export class ScEmptyState extends LitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      text-align: center;
      padding: var(--sc-space-2xl) var(--sc-space-xl);
    }

    .icon {
      width: 3.5rem;
      height: 3.5rem;
      margin-bottom: var(--sc-space-md);
      color: var(--sc-text-faint);
      animation: sc-bounce-in var(--sc-duration-normal) var(--sc-ease-out) both;
    }

    .icon svg {
      width: 100%;
      height: 100%;
    }

    .heading {
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin-bottom: var(--sc-space-xs);
    }

    .description {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      max-width: 20rem;
      line-height: var(--sc-leading-relaxed);
      animation: sc-fade-in var(--sc-duration-normal) var(--sc-ease-out) var(--sc-duration-fast)
        both;
    }

    .slot {
      margin-top: var(--sc-space-md);
    }
    @media (prefers-reduced-motion: reduce) {
      .icon,
      .description {
        animation: none !important;
      }
    }
  `;

  @property({ attribute: false }) icon: TemplateResult | null = null;
  @property({ type: String }) heading = "";
  @property({ type: String }) description = "";

  override render() {
    return html`
      ${this.icon ? html`<div class="icon" aria-hidden="true">${this.icon}</div>` : nothing}
      ${this.heading ? html`<h2 class="heading">${this.heading}</h2>` : nothing}
      ${this.description ? html`<p class="description">${this.description}</p>` : nothing}
      <div class="slot"><slot></slot></div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-empty-state": ScEmptyState;
  }
}
