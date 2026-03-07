import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./sc-button.js";

@customElement("sc-error-boundary")
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
      padding: var(--sc-space-2xl) var(--sc-space-xl);
      min-height: 12.5rem;
    }

    .icon {
      width: 3.5rem;
      height: 3.5rem;
      margin-bottom: var(--sc-space-md);
      color: var(--sc-error);
    }

    .icon svg {
      width: 100%;
      height: 100%;
    }

    .heading {
      font-size: var(--sc-text-lg);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      margin: 0 0 var(--sc-space-xs);
    }

    .description {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      max-width: 20rem;
      line-height: var(--sc-leading-relaxed);
      margin: 0 0 var(--sc-space-lg);
    }

    .action {
      margin-top: var(--sc-space-sm);
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
            <sc-button variant="primary" @click=${this._onRetry}>Try again</sc-button>
          </div>
        </div>
      `;
    }

    return html`<div class="slot"><slot></slot></div>`;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-error-boundary": ScErrorBoundary;
  }
}
