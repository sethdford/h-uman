import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

@customElement("hu-web-search-result")
export class ScWebSearchResult extends LitElement {
  @property({ type: String }) query = "";
  @property({ type: Array }) sites: string[] = [];
  @property({ type: Array }) sources: Array<{ title: string; url: string }> = [];

  @state() private _expanded = false;

  static override styles = css`
    :host {
      display: block;
      contain: layout style;
    }

    .search-card {
      background: color-mix(in srgb, var(--hu-accent-secondary, var(--hu-accent)) 4%, transparent);
      border-left: 0.125rem solid
        color-mix(in srgb, var(--hu-accent-secondary, var(--hu-accent)) 30%, transparent);
      border-radius: var(--hu-radius-md);
      overflow: hidden;
      font-family: var(--hu-font);
    }

    .header {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      padding: var(--hu-space-sm) var(--hu-space-md);
      cursor: pointer;
      user-select: none;
      background: transparent;
      border: none;
      width: 100%;
      text-align: left;
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      transition: background-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .header:hover {
      background: color-mix(in srgb, var(--hu-accent-secondary, var(--hu-accent)) 8%, transparent);
    }

    .header:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .search-icon {
      display: flex;
      align-items: center;
      width: 1rem;
      height: 1rem;
      flex-shrink: 0;
      color: var(--hu-accent-secondary, var(--hu-accent));
    }

    .search-icon svg {
      width: 100%;
      height: 100%;
    }

    .label {
      flex: 1;
      min-width: 0;
    }

    .site-count {
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
    }

    .query-text {
      margin-inline-start: var(--hu-space-xs);
      font-style: italic;
      color: var(--hu-text-muted);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .caret {
      display: inline-flex;
      width: 0.75rem;
      height: 0.75rem;
      flex-shrink: 0;
      transition: transform var(--hu-duration-fast) var(--hu-ease-out);
    }

    .caret.open {
      transform: rotate(90deg);
    }

    .caret svg {
      width: 100%;
      height: 100%;
    }

    .detail {
      overflow: hidden;
      max-height: 0;
      opacity: 0;
      transform: translateY(-0.25rem);
      transition:
        max-height var(--hu-duration-normal) var(--hu-ease-out),
        opacity var(--hu-duration-normal) var(--hu-ease-out),
        transform var(--hu-duration-normal) var(--hu-ease-out),
        padding var(--hu-duration-normal) var(--hu-ease-out);
      padding: 0 var(--hu-space-md);
    }

    .detail.open {
      max-height: 30rem;
      opacity: 1;
      transform: translateY(0);
      padding: 0 var(--hu-space-md) var(--hu-space-md);
    }

    .query-section {
      margin-bottom: var(--hu-space-sm);
    }

    .section-label {
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text-muted);
      text-transform: uppercase;
      letter-spacing: 0.05em;
      margin-bottom: var(--hu-space-2xs);
    }

    .query-value {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      font-style: italic;
    }

    .sites-list {
      display: flex;
      flex-wrap: wrap;
      gap: var(--hu-space-xs);
      margin-bottom: var(--hu-space-sm);
    }

    .site-badge {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-2xs);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      background: var(--hu-bg-inset);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-xs);
      color: var(--hu-text);
    }

    .site-favicon {
      width: 0.75rem;
      height: 0.75rem;
      border-radius: var(--hu-radius-sm);
    }

    .sources-list {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
    }

    .source-item {
      display: flex;
      align-items: flex-start;
      gap: var(--hu-space-xs);
    }

    .source-bullet {
      flex-shrink: 0;
      width: var(--hu-space-xs);
      height: var(--hu-space-xs);
      border-radius: 50%;
      background: var(--hu-accent-secondary, var(--hu-accent));
      margin-top: 0.375rem;
    }

    .source-link {
      font-size: var(--hu-text-sm);
      color: var(--hu-accent);
      text-decoration: none;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      max-width: 100%;
    }

    .source-link:hover {
      text-decoration: underline;
    }

    @media (prefers-reduced-motion: reduce) {
      .caret,
      .detail {
        transition: none;
      }
      .detail.open {
        transform: none;
      }
    }
  `;

  private _toggle(): void {
    this._expanded = !this._expanded;
  }

  private _getDomain(url: string): string {
    try {
      return new URL(url).hostname.replace(/^www\./, "");
    } catch {
      return url;
    }
  }

  override render() {
    const siteCount = this.sites.length || this.sources.length;
    const sitesLabel = siteCount === 1 ? "1 site" : `${siteCount} sites`;

    return html`
      <div class="search-card" role="region" aria-label="Web search results">
        <button
          class="header"
          @click=${this._toggle}
          aria-expanded=${this._expanded}
          aria-controls="search-detail"
        >
          <span class="search-icon" aria-hidden="true">${icons["magnifying-glass"]}</span>
          <span class="label">
            <span class="site-count">Searched ${sitesLabel}</span>
            ${this.query ? html`<span class="query-text">“${this.query}”</span>` : nothing}
          </span>
          <span class="caret ${this._expanded ? "open" : ""}">${icons["caret-right"]}</span>
        </button>
        <div id="search-detail" class="detail ${this._expanded ? "open" : ""}">
          ${
            this.query
              ? html`
                  <div class="query-section">
                    <div class="section-label">Query</div>
                    <div class="query-value">${this.query}</div>
                  </div>
                `
              : nothing
          }
          ${
            this.sites.length > 0
              ? html`
                  <div class="query-section">
                    <div class="section-label">Sites searched</div>
                    <div class="sites-list">
                      ${this.sites.map(
                        (site) => html`
                          <span class="site-badge">
                            <img
                              class="site-favicon"
                              src="https://www.google.com/s2/favicons?domain=${this._getDomain(
                                site,
                              )}&sz=16"
                              alt=""
                              loading="lazy"
                              width="12"
                              height="12"
                            />
                            ${this._getDomain(site)}
                          </span>
                        `,
                      )}
                    </div>
                  </div>
                `
              : nothing
          }
          ${
            this.sources.length > 0
              ? html`
                  <div class="query-section">
                    <div class="section-label">Sources</div>
                    <ul class="sources-list">
                      ${this.sources.map(
                        (source) => html`
                          <li class="source-item">
                            <span class="source-bullet"></span>
                            <a
                              class="source-link"
                              href=${source.url}
                              target="_blank"
                              rel="noopener noreferrer"
                              >${source.title || this._getDomain(source.url)}</a
                            >
                          </li>
                        `,
                      )}
                    </ul>
                  </div>
                `
              : nothing
          }
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-web-search-result": ScWebSearchResult;
  }
}
