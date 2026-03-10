import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./hu-button.js";

@customElement("hu-error-boundary")
export class ScErrorBoundary extends LitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      flex: 1;
      min-height: 0;
    }

    .fallback {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      text-align: center;
      padding: var(--hu-space-2xl) var(--hu-space-xl);
      min-height: 12.5rem;
    }

    .icon {
      width: 3.5rem;
      height: 3.5rem;
      margin-bottom: var(--hu-space-md);
      color: var(--hu-error);
    }

    .icon svg {
      width: 100%;
      height: 100%;
    }

    .heading {
      font-size: var(--hu-text-lg);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      margin: 0 0 var(--hu-space-xs);
    }

    .description {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      max-width: 20rem;
      line-height: var(--hu-leading-relaxed);
      margin: 0 0 var(--hu-space-lg);
    }

    .action {
      margin-top: var(--hu-space-sm);
    }

    .slot {
      display: contents;
    }
  `;

  @property({ attribute: false }) error: Error | null = null;

  private _onRetry(): void {
    this.dispatchEvent(new CustomEvent("retry", { bubbles: true, composed: true }));
  }

  override render() {
    if (this.error) {
      return html`
        <div class="fallback" role="alert" aria-live="assertive">
          <div class="icon" aria-hidden="true">${icons.warning}</div>
          <h2 class="heading">Something went wrong</h2>
          <p class="description">
            An error occurred while rendering. You can try again or refresh the page.
          </p>
          <div class="action">
            <hu-button variant="primary" @click=${this._onRetry}>Try again</hu-button>
          </div>
        </div>
      `;
    }

    return html`<div class="slot"><slot></slot></div>`;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-error-boundary": ScErrorBoundary;
  }
}
