import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

@customElement("hu-slider")
export class ScSlider extends LitElement {
  @property({ type: Number }) value = 50;
  @property({ type: Number }) min = 0;
  @property({ type: Number }) max = 100;
  @property({ type: Number }) step = 1;
  @property({ type: String }) label = "";
  @property({ type: Boolean }) disabled = false;
  @property({ type: Boolean }) showValue = true;

  @state() private _active = false;

  @state() private _sliderId = `hu-slider-${Math.random().toString(36).slice(2, 11)}`;

  static override styles = css`
    :host {
      display: block;
    }

    .wrapper {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
    }

    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: var(--hu-space-sm);
    }

    .label {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-medium);
    }

    .value {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-medium);
      font-variant-numeric: tabular-nums;
    }

    .track-wrap {
      position: relative;
      height: 1.25rem;
      display: flex;
      align-items: center;
      cursor: pointer;
    }

    .track-wrap[aria-disabled="true"] {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    .track {
      flex: 1;
      height: var(--hu-space-xs);
      background: var(--hu-border-subtle);
      border-radius: var(--hu-radius-full);
      position: relative;
      overflow: visible;
    }

    .track-fill {
      position: absolute;
      left: 0;
      top: 0;
      bottom: 0;
      background: var(--hu-accent);
      border-radius: var(--hu-radius-full);
      transition: width var(--hu-duration-fast) var(--hu-ease-out);
      min-width: 0;
    }

    .thumb {
      position: absolute;
      top: 50%;
      left: var(--thumb-percent, 0%);
      width: 1.25rem;
      height: 1.25rem;
      margin-left: calc(-1 * var(--hu-space-md));
      margin-top: calc(-1 * var(--hu-space-md));
      background: var(--hu-bg-surface);
      border-radius: var(--hu-radius-full);
      box-shadow: var(--hu-shadow-md);
      border: 2px solid transparent;
      transform: scale(1);
      transition:
        left var(--hu-duration-fast) var(--hu-ease-out),
        transform var(--hu-duration-fast) var(--hu-ease-out),
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out);
      pointer-events: none;
    }

    .track-wrap:hover:not([aria-disabled="true"]) .thumb {
      transform: scale(1.1);
    }

    .track-wrap.active .thumb {
      transform: scale(0.95);
    }

    .track-wrap:has(input:focus-visible) {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
      border-radius: var(--hu-radius-full);
    }

    input:focus-visible ~ .track .thumb {
      border-color: var(--hu-accent);
      box-shadow: 0 0 0 var(--hu-focus-ring-width) var(--hu-focus-ring);
    }

    @media (prefers-reduced-motion: reduce) {
      .track-fill,
      .thumb {
        transition: none;
      }
    }

    input {
      position: absolute;
      width: 100%;
      height: 100%;
      opacity: 0;
      cursor: pointer;
      margin: 0;
      padding: 0;
    }

    input:disabled {
      cursor: not-allowed;
    }
  `;

  private get _percent(): number {
    const range = this.max - this.min;
    if (range <= 0) return 0;
    const val = Math.max(this.min, Math.min(this.max, this.value));
    return ((val - this.min) / range) * 100;
  }

  private _onInput(e: Event): void {
    const target = e.target as HTMLInputElement;
    this.value = parseFloat(target.value) || this.min;
    this.dispatchEvent(
      new CustomEvent("hu-change", {
        bubbles: true,
        composed: true,
        detail: { value: this.value },
      }),
    );
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (
      e.key !== "ArrowLeft" &&
      e.key !== "ArrowRight" &&
      e.key !== "ArrowUp" &&
      e.key !== "ArrowDown"
    )
      return;
    e.preventDefault();
    const step = e.key === "ArrowLeft" || e.key === "ArrowDown" ? -this.step : this.step;
    let next = this.value + step;
    next = Math.round(next / this.step) * this.step;
    next = Math.max(this.min, Math.min(this.max, next));
    if (next !== this.value) {
      this.value = next;
      this.dispatchEvent(
        new CustomEvent("hu-change", {
          bubbles: true,
          composed: true,
          detail: { value: this.value },
        }),
      );
    }
  }

  private _onPointerDown(): void {
    if (this.disabled) return;
    this._active = true;
  }

  private _onPointerUp = (): void => {
    this._active = false;
  };

  override connectedCallback(): void {
    super.connectedCallback();
    window.addEventListener("pointerup", this._onPointerUp);
    window.addEventListener("pointercancel", this._onPointerUp);
  }

  override disconnectedCallback(): void {
    window.removeEventListener("pointerup", this._onPointerUp);
    window.removeEventListener("pointercancel", this._onPointerUp);
    super.disconnectedCallback();
  }

  override render() {
    const ariaLabel = this.label || "Slider";
    return html`
      <div class="wrapper">
        <div class="header">
          ${this.label
            ? html`<label class="label" for=${this._sliderId}>${this.label}</label>`
            : html`<span class="label" id=${`${this._sliderId}-label`}>${ariaLabel}</span>`}
          ${this.showValue ? html`<span class="value">${this.value}</span>` : null}
        </div>
        <div
          class="track-wrap ${this._active ? "active" : ""}"
          aria-disabled=${this.disabled ? "true" : "false"}
          @pointerdown=${this._onPointerDown}
        >
          <input
            id=${this._sliderId}
            type="range"
            min=${this.min}
            max=${this.max}
            step=${this.step}
            .value=${String(this.value)}
            ?disabled=${this.disabled}
            role="slider"
            aria-valuemin=${this.min}
            aria-valuemax=${this.max}
            aria-valuenow=${this.value}
            aria-label=${ariaLabel}
            aria-labelledby=${this.label ? undefined : `${this._sliderId}-label`}
            @input=${this._onInput}
            @keydown=${this._onKeyDown}
          />
          <div class="track" style="--thumb-percent: ${this._percent}%">
            <div class="track-fill" style="width: ${this._percent}%"></div>
            <div class="thumb"></div>
          </div>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-slider": ScSlider;
  }
}
