import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";

const FAVICON_API = "https://www.google.com/s2/favicons?domain=";

/** Metadata API for link previews. Uses Microlink free tier (no key required). */
const METADATA_API = "https://api.microlink.io";

@customElement("sc-link-preview")
export class ScLinkPreview extends LitElement {
  @property({ type: String }) url = "";
  @property({ type: String }) title = "";
  @property({ type: String }) description = "";
  @property({ type: String }) image = "";
  @property({ type: String }) domain = "";
  @property({ type: Boolean }) loading = false;
  @property({ type: Boolean }) failed = false;

  @state() private _fetched = false;
  private _observer: IntersectionObserver | null = null;
  private _lastUrl = "";

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("url") && this.url !== this._lastUrl) {
      this._lastUrl = this.url;
      this._fetched = false;
      this._observer?.disconnect();
      this._observer = null;
      this._setupIntersectionObserver();
    }
  }

  static override styles = css`
    @keyframes sc-preview-enter {
      from {
        opacity: 0;
        transform: translateY(var(--sc-space-xs));
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    :host {
      display: block;
      max-width: 25rem;
    }

    .card {
      display: flex;
      flex-direction: column;
      gap: 0;
      padding: 0;
      background: var(--sc-surface-container);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-lg);
      text-decoration: none;
      color: inherit;
      box-shadow: var(--sc-shadow-sm);
      overflow: hidden;
      animation: sc-preview-enter var(--sc-duration-fast) var(--sc-ease-out);
      transition:
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out),
        transform var(--sc-duration-fast) var(--sc-ease-spring, var(--sc-ease-out));
    }

    .card:hover {
      border-color: var(--sc-accent);
      box-shadow: var(--sc-shadow-md);
      transform: translateY(calc(-1 * var(--sc-space-2xs)));
    }

    .card:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-accent);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .card-inner {
      display: flex;
      align-items: stretch;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm);
    }

    .thumb {
      width: var(--sc-space-4xl);
      min-width: var(--sc-space-4xl);
      height: var(--sc-space-4xl);
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
      flex: 1;
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
      width: 1.25rem;
      height: 1.25rem;
      border-radius: var(--sc-radius-xs);
      flex-shrink: 0;
    }

    .domain {
      font-family: var(--sc-font);
      font-size: var(--sc-text-2xs, 0.625rem);
      color: var(--sc-text-faint);
    }

    .fallback-link {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      padding: var(--sc-space-sm) var(--sc-space-md);
      color: var(--sc-accent);
      text-decoration: none;
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      border-radius: var(--sc-radius-md);
      transition: background var(--sc-duration-fast) var(--sc-ease-out);
    }

    .fallback-link:hover {
      background: var(--sc-hover-overlay);
    }

    .fallback-link:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-accent);
      outline-offset: var(--sc-focus-ring-offset);
    }

    .skeleton-card {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm);
      background: var(--sc-surface-container);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-sm);
    }

    .skeleton-row {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
    }

    .skeleton-thumb {
      width: var(--sc-space-4xl);
      height: var(--sc-space-4xl);
      border-radius: var(--sc-radius-md);
      background: linear-gradient(
        105deg,
        var(--sc-bg-elevated) 25%,
        var(--sc-bg-overlay) 37%,
        var(--sc-bg-elevated) 63%
      );
      background-size: 400% 100%;
      animation: sc-skel-shimmer var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }

    .skeleton-text {
      flex: 1;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .skeleton-line {
      height: var(--sc-text-sm);
      border-radius: var(--sc-radius-sm);
      background: linear-gradient(
        105deg,
        var(--sc-bg-elevated) 25%,
        var(--sc-bg-overlay) 37%,
        var(--sc-bg-elevated) 63%
      );
      background-size: 400% 100%;
      animation: sc-skel-shimmer var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }

    .skeleton-line.short {
      width: 60%;
    }

    .skeleton-line.domain {
      width: 40%;
      height: var(--sc-text-xs);
    }

    @keyframes sc-skel-shimmer {
      0% {
        background-position: 100% 50%;
      }
      100% {
        background-position: 0% 50%;
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .card {
        animation: none;
      }
      .card:hover {
        transform: none;
      }
      .skeleton-thumb,
      .skeleton-line {
        animation: none;
      }
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    this._setupIntersectionObserver();
  }

  override disconnectedCallback(): void {
    this._observer?.disconnect();
    this._observer = null;
    super.disconnectedCallback();
  }

  private _needsFetch(): boolean {
    return (
      !!this.url &&
      !this.title &&
      !this.description &&
      !this.image &&
      !this.loading &&
      !this.failed &&
      !this._fetched
    );
  }

  private _setupIntersectionObserver(): void {
    if (!this._needsFetch()) return;
    this._observer = new IntersectionObserver(
      (entries) => {
        if (!entries[0]?.isIntersecting) return;
        this._observer?.disconnect();
        this._observer = null;
        this._fetched = true;
        this._fetchMetadata();
      },
      { rootMargin: "200px" },
    );
    this._observer.observe(this);
  }

  private async _fetchMetadata(): Promise<void> {
    if (!this.url) return;
    this.loading = true;
    this.requestUpdate();
    try {
      const res = await fetch(
        `${METADATA_API}?url=${encodeURIComponent(this.url)}&screenshot=false&video=false`,
      );
      const data = (await res.json()) as {
        data?: {
          title?: string;
          description?: string;
          image?: { url?: string };
          publisher?: string;
        };
        status?: string;
      };
      if (data?.status === "success" && data.data) {
        const d = data.data;
        if (d.title) this.title = d.title;
        if (d.description) this.description = d.description;
        if (d.image?.url) this.image = d.image.url;
        if (d.publisher) this.domain = d.publisher;
      } else {
        this.failed = true;
      }
    } catch {
      this.failed = true;
    } finally {
      this.loading = false;
      this.requestUpdate();
    }
  }

  private _getDomain(): string {
    if (this.domain) return this.domain;
    try {
      return new URL(this.url).hostname.replace(/^www\./, "");
    } catch {
      return this.url;
    }
  }

  private _getFaviconUrl(): string {
    const d = this._getDomain();
    if (!d || d === this.url) return "";
    return `${FAVICON_API}${encodeURIComponent(d)}&sz=32`;
  }

  override render() {
    if (!this.url) return nothing;

    if (this.loading) {
      return html`
        <div class="skeleton-card" role="status" aria-busy="true" aria-label="Loading link preview">
          <div class="skeleton-row">
            <div class="skeleton-thumb"></div>
            <div class="skeleton-text">
              <div class="skeleton-line short"></div>
              <div class="skeleton-line"></div>
              <div class="skeleton-line domain"></div>
            </div>
          </div>
        </div>
      `;
    }

    if (this.failed || (!this.title && !this.description && !this.image)) {
      const faviconUrl = this._getFaviconUrl();
      return html`
        <a
          class="fallback-link"
          href=${this.url}
          target="_blank"
          rel="noopener noreferrer"
          aria-label=${`Open link: ${this._getDomain()}`}
        >
          ${faviconUrl ? html`<img class="fav" src=${faviconUrl} alt="" />` : nothing}
          <span>${this.url}</span>
        </a>
      `;
    }

    const faviconUrl = this._getFaviconUrl();
    return html`
      <a
        class="card"
        href=${this.url}
        target="_blank"
        rel="noopener noreferrer"
        aria-label=${this.title || this._getDomain()}
      >
        <div class="card-inner">
          ${this.image
            ? html`<img class="thumb" src=${this.image} alt="" loading="lazy" />`
            : nothing}
          <div class="text">
            ${this.title ? html`<span class="title">${this.title}</span>` : nothing}
            ${this.description ? html`<span class="desc">${this.description}</span>` : nothing}
            <div class="domain-row">
              ${faviconUrl ? html`<img class="fav" src=${faviconUrl} alt="" />` : nothing}
              <span class="domain">${this._getDomain()}</span>
            </div>
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
