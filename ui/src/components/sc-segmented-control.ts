import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

type SegmentSize = "sm" | "md" | "lg";

export interface SegmentOption {
  value: string;
  label: string;
}

@customElement("sc-segmented-control")
export class ScSegmentedControl extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: Array }) options: SegmentOption[] = [];
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) size: SegmentSize = "md";

  static override styles = css`
    :host {
      display: inline-flex;
    }

    .container {
      display: inline-flex;
      align-items: stretch;
      background: var(--sc-bg-inset);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-lg);
      padding: 2px;
      position: relative;
      gap: 0;
    }

    .container[aria-disabled="true"] {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }

    .indicator {
      position: absolute;
      top: 2px;
      bottom: 2px;
      left: 2px;
      background: var(--sc-accent);
      border-radius: calc(var(--sc-radius-lg) - 2px);
      transition: transform var(--sc-duration-normal) var(--sc-ease-out);
      pointer-events: none;
      z-index: 0;
    }

    @media (prefers-reduced-motion: reduce) {
      .indicator {
        transition: none;
      }
    }

    .segment {
      position: relative;
      z-index: 1;
      flex: 1;
      min-width: 0;
      padding: var(--sc-space-xs) var(--sc-space-md);
      font-family: var(--sc-font);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text-muted);
      background: none;
      border: none;
      border-radius: calc(var(--sc-radius-lg) - 2px);
      cursor: pointer;
      transition: color var(--sc-duration-fast) var(--sc-ease-out);
    }

    .segment:hover:not(:disabled):not(.active) {
      color: var(--sc-text);
    }

    .segment.active {
      color: var(--sc-on-accent);
    }

    .segment:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-focus-ring);
      outline-offset: 2px;
    }

    .segment:disabled {
      cursor: not-allowed;
    }

    .segment.size-sm {
      font-size: var(--sc-text-sm);
      padding: var(--sc-space-2xs) var(--sc-space-sm);
    }

    .segment.size-md {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-xs) var(--sc-space-md);
    }

    .segment.size-lg {
      font-size: var(--sc-text-base);
      padding: var(--sc-space-sm) var(--sc-space-lg);
    }
  `;

  private get _activeIndex(): number {
    return Math.max(
      0,
      this.options.findIndex((o) => o.value === this.value),
    );
  }

  private get _indicatorStyle(): string {
    if (this.options.length === 0) return "";
    const idx = this._activeIndex;
    const count = this.options.length;
    const width = 100 / count;
    const left = (idx / count) * 100;
    return `width: ${width}%; transform: translateX(${idx * 100}%);`;
  }

  private _onSelect(value: string): void {
    if (this.disabled) return;
    this.value = value;
    this.dispatchEvent(
      new CustomEvent("sc-change", {
        bubbles: true,
        composed: true,
        detail: { value },
      }),
    );
  }

  private _onKeyDown(e: KeyboardEvent, currentIdx: number): void {
    if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
    e.preventDefault();
    const dir = e.key === "ArrowLeft" ? -1 : 1;
    const nextIdx = Math.max(0, Math.min(this.options.length - 1, currentIdx + dir));
    const next = this.options[nextIdx];
    if (next) this._onSelect(next.value);
  }

  override render() {
    if (this.options.length === 0) return html``;

    return html`
      <div class="container" role="tablist" aria-disabled=${this.disabled ? "true" : "false"}>
        <div class="indicator" style=${this._indicatorStyle} aria-hidden="true"></div>
        ${this.options.map(
          (opt, i) => html`
            <button
              type="button"
              class="segment size-${this.size} ${opt.value === this.value ? "active" : ""}"
              role="tab"
              aria-selected=${opt.value === this.value}
              aria-label=${opt.label}
              ?disabled=${this.disabled}
              @click=${() => this._onSelect(opt.value)}
              @keydown=${(e: KeyboardEvent) => this._onKeyDown(e, i)}
            >
              ${opt.label}
            </button>
          `,
        )}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-segmented-control": ScSegmentedControl;
  }
}
