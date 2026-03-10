import { LitElement, html, css, nothing, type TemplateResult } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("hu-empty-state")
export class ScEmptyState extends LitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      text-align: center;
      padding: var(--hu-space-2xl) var(--hu-space-xl);
    }

    .icon {
      width: 3.5rem;
      height: 3.5rem;
      margin-bottom: var(--hu-space-md);
      color: var(--hu-text-faint);
      animation: hu-bounce-in var(--hu-duration-normal) var(--hu-ease-out) both;
    }

    .icon svg {
      width: 100%;
      height: 100%;
    }

    .heading {
      font-size: var(--hu-text-lg);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      margin-bottom: var(--hu-space-xs);
    }

    .description {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      max-width: 20rem;
      line-height: var(--hu-leading-relaxed);
      animation: hu-fade-in var(--hu-duration-normal) var(--hu-ease-out) var(--hu-duration-fast)
        both;
    }

    .slot {
      margin-top: var(--hu-space-md);
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
    "hu-empty-state": ScEmptyState;
  }
}
