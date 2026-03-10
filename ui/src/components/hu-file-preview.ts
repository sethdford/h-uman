import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

export interface FilePreviewItem {
  name: string;
  size: number;
  type: string;
  dataUrl?: string;
}

@customElement("hu-file-preview")
export class ScFilePreview extends LitElement {
  @property({ type: Array }) files: FilePreviewItem[] = [];

  static override styles = css`
    :host {
      display: block;
    }

    .grid {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: var(--hu-space-sm);
      padding: var(--hu-space-sm) 0;
    }

    @media (max-width: 480px) /* --hu-breakpoint-sm */ {
      .grid {
        grid-template-columns: repeat(2, 1fr);
      }
    }

    .card {
      position: relative;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      min-height: 5rem;
      padding: var(--hu-space-sm);
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      overflow: hidden;
      box-shadow: var(--hu-shadow-xs);
      transition:
        box-shadow var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out);
    }

    .card:hover {
      box-shadow: var(--hu-shadow-sm);
      transform: translateY(-2px);
    }

    .card-image {
      width: 100%;
      height: 5rem;
      object-fit: cover;
      border-radius: var(--hu-radius-sm);
      box-shadow: inset 0 1px 0.1875rem color-mix(in srgb, var(--hu-text) 15%, transparent);
    }

    .card-icon {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 2.5rem;
      height: 2.5rem;
      color: var(--hu-text-muted);
    }

    .card-icon svg {
      width: var(--hu-icon-lg);
      height: var(--hu-icon-lg);
    }

    .card-name {
      font-size: var(--hu-text-xs);
      font-family: var(--hu-font);
      color: var(--hu-text);
      text-align: center;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      max-width: 100%;
      margin-top: var(--hu-space-2xs);
    }

    .card-size {
      font-size: var(--hu-text-2xs, 10px);
      color: var(--hu-text-muted);
      margin-top: var(--hu-space-2xs);
    }

    .remove-btn {
      position: absolute;
      top: var(--hu-space-2xs);
      right: var(--hu-space-2xs);
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--hu-icon-lg);
      height: var(--hu-icon-lg);
      padding: 0;
      background: color-mix(in srgb, var(--hu-bg-surface) 90%, transparent);
      backdrop-filter: blur(8px);
      -webkit-backdrop-filter: blur(8px);
      border: none;
      border-radius: var(--hu-radius-full);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .remove-btn:hover {
      color: var(--hu-error);
      background: var(--hu-error-dim);
    }

    .remove-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .remove-btn svg {
      width: 14px;
      height: 14px;
    }

    @media (prefers-reduced-motion: reduce) {
      .card,
      .remove-btn {
        transition: none;
      }
    }
  `;

  private _formatSize(bytes: number): string {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  }

  private _isImage(type: string): boolean {
    return type.startsWith("image/");
  }

  private _onRemove(e: Event, index: number): void {
    e.stopPropagation();
    this.dispatchEvent(
      new CustomEvent("hu-file-remove", {
        bubbles: true,
        composed: true,
        detail: { index },
      }),
    );
  }

  override render() {
    if (this.files.length === 0) return html``;
    return html`
      <div class="grid" role="list">
        ${this.files.map(
          (f, i) => html`
            <div class="card" role="listitem">
              <button
                type="button"
                class="remove-btn"
                aria-label="Remove ${f.name}"
                @click=${(e: Event) => this._onRemove(e, i)}
              >
                ${icons["x-circle"]}
              </button>
              ${f.dataUrl && this._isImage(f.type)
                ? html`
                    <img class="card-image" src=${f.dataUrl} alt=${f.name} loading="lazy" />
                    <span class="card-size">${this._formatSize(f.size)}</span>
                  `
                : html`
                    <div class="card-icon">${icons["file-text"]}</div>
                    <span class="card-name">${f.name}</span>
                    <span class="card-size">${this._formatSize(f.size)}</span>
                  `}
            </div>
          `,
        )}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-file-preview": ScFilePreview;
  }
}
