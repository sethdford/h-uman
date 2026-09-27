import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

type SegmentSize = "sm" | "md" | "lg";

export interface SegmentOption {
  value: string;
  label: string;
}

@customElement("hu-segmented-control")
export class ScSegmentedControl extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: Array }) options: SegmentOption[] = [];
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) size: SegmentSize = "md";
  /** Accessible name for the radiogroup (replaces misleading tab/tablist semantics). */
  @property({ type: String, attribute: "aria-label" }) ariaLabel = "Segmented control";

  static override styles = css`
    :host {
      display: inline-flex;
    }

    .container {
      display: inline-flex;
      align-items: stretch;
      background: var(--hu-bg-inset);
      border: 1px solid var(--hu-border-subtle);
      border-radius: var(--hu-radius-lg);
      padding: var(--hu-space-2xs);
      position: relative;
      gap: 0;
    }

    .container[aria-disabled="true"] {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    .indicator {
      position: absolute;
      top: var(--hu-space-2xs);
      bottom: var(--hu-space-2xs);
      left: var(--hu-space-2xs);
      background: var(--hu-accent);
      border-radius: calc(var(--hu-radius-lg) - var(--hu-space-2xs));
      transition: transform var(--hu-duration-normal) var(--hu-spring-bounce);
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
      padding: var(--hu-space-xs) var(--hu-space-md);
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text-muted);
      background: none;
      border: none;
      border-radius: calc(var(--hu-radius-lg) - var(--hu-space-2xs));
      cursor: pointer;
      transition: color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .segment:hover:not(:disabled):not(.active) {
      color: var(--hu-text);
    }

    .segment.active {
      color: var(--hu-on-accent);
    }

    .segment:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: 2px;
    }

    .segment:disabled {
      cursor: not-allowed;
    }

    .segment.size-sm {
      font-size: var(--hu-text-sm);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
    }

    .segment.size-md {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-xs) var(--hu-space-md);
    }

    .segment.size-lg {
      font-size: var(--hu-text-base);
      padding: var(--hu-space-sm) var(--hu-space-lg);
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
    return `width: ${width}%; transform: translateX(${idx * 100}%);`;
  }

  private _onSelect(value: string): void {
    if (this.disabled) return;
    this.value = value;
    this.dispatchEvent(
      new CustomEvent("hu-change", {
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
      <div
        class="container"
        role="radiogroup"
        aria-label=${this.ariaLabel}
        aria-disabled=${this.disabled ? "true" : "false"}
      >
        <div class="indicator" style=${this._indicatorStyle} aria-hidden="true"></div>
        ${this.options.map(
          (opt, i) => html`
            <button
              type="button"
              class="segment size-${this.size} ${opt.value === this.value ? "active" : ""}"
              role="radio"
              aria-checked=${opt.value === this.value ? "true" : "false"}
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
    "hu-segmented-control": ScSegmentedControl;
  }
}
