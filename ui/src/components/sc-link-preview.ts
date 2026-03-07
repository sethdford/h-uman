import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-link-preview")
export class ScLinkPreview extends LitElement {
  @property({ type: String }) url = "";
  @property({ type: String }) title = "";
  @property({ type: String }) description = "";
  @property({ type: String }) favicon = "";
  @property({ type: String }) image = "";

  static override styles = css`
    @keyframes sc-preview-enter {
      from {
        opacity: 0;
        transform: translateY(4px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    :host {
      display: block;
      max-width: 20rem;
    }

    .card {
      display: flex;
      align-items: stretch;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-lg);
      text-decoration: none;
      color: inherit;
      animation: sc-preview-enter 200ms var(--sc-ease-out);
      transition:
        border-color var(--sc-duration-fast),
        box-shadow var(--sc-duration-fast);
    }

    .card:hover {
      border-color: var(--sc-border);
      box-shadow: var(--sc-shadow-xs);
    }

    .card:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .thumb {
      width: 48px;
      min-width: 48px;
      height: 48px;
      border-radius: var(--sc-radius-md);
      object-fit: cover;
      flex-shrink: 0;
    }

    .text {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
      min-width: 0;
      justify-content: center;
    }

    .title {
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .desc {
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      display: -webkit-box;
      -webkit-line-clamp: 2;
      -webkit-box-orient: vertical;
      overflow: hidden;
    }

    .domain-row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-2xs);
    }

    .fav {
      width: 14px;
      height: 14px;
      border-radius: 2px;
    }

    .domain {
      font-family: var(--sc-font);
      font-size: var(--sc-text-2xs, 10px);
      color: var(--sc-text-faint);
    }

    @media (prefers-reduced-motion: reduce) {
      .card {
        animation: none;
      }
    }
  `;

  private _getDomain(): string {
    try {
      return new URL(this.url).hostname.replace(/^www\./, "");
    } catch {
      return this.url;
    }
  }

  override render() {
    if (!this.url) return nothing;

    return html`
      <a
        class="card"
        href=${this.url}
        target="_blank"
        rel="noopener noreferrer"
        aria-label=${this.title || this._getDomain()}
      >
        ${this.image
          ? html`<img class="thumb" src=${this.image} alt="" loading="lazy" />`
          : nothing}
        <div class="text">
          ${this.title ? html`<span class="title">${this.title}</span>` : nothing}
          ${this.description ? html`<span class="desc">${this.description}</span>` : nothing}
          <div class="domain-row">
            ${this.favicon ? html`<img class="fav" src=${this.favicon} alt="" />` : nothing}
            <span class="domain">${this._getDomain()}</span>
          </div>
        </div>
      </a>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-link-preview": ScLinkPreview;
  }
}
