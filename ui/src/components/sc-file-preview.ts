import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

export interface FilePreviewItem {
  name: string;
  size: number;
  type: string;
  dataUrl?: string;
}

@customElement("sc-file-preview")
export class ScFilePreview extends LitElement {
  @property({ type: Array }) files: FilePreviewItem[] = [];

  static override styles = css`
    :host {
      display: block;
    }

    .grid {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) 0;
    }

    @media (max-width: 480px) {
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
      min-height: 80px;
      padding: var(--sc-space-sm);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      overflow: hidden;
    }

    .card-image {
      width: 100%;
      height: 80px;
      object-fit: cover;
      border-radius: var(--sc-radius-sm);
    }

    .card-icon {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 40px;
      height: 40px;
      color: var(--sc-text-muted);
    }

    .card-icon svg {
      width: 24px;
      height: 24px;
    }

    .card-name {
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      color: var(--sc-text);
      text-align: center;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      max-width: 100%;
      margin-top: var(--sc-space-2xs);
    }

    .card-size {
      font-size: var(--sc-text-2xs, 10px);
      color: var(--sc-text-muted);
      margin-top: var(--sc-space-2xs);
    }

    .remove-btn {
      position: absolute;
      top: var(--sc-space-2xs);
      right: var(--sc-space-2xs);
      display: flex;
      align-items: center;
      justify-content: center;
      width: 24px;
      height: 24px;
      padding: 0;
      background: color-mix(in srgb, var(--sc-bg) 90%, transparent);
      border: none;
      border-radius: var(--sc-radius-full);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        color var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }

    .remove-btn:hover {
      color: var(--sc-error);
      background: var(--sc-error-dim);
    }

    .remove-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .remove-btn svg {
      width: 14px;
      height: 14px;
    }

    @media (prefers-reduced-motion: reduce) {
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
      new CustomEvent("sc-file-remove", {
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
    "sc-file-preview": ScFilePreview;
  }
}
