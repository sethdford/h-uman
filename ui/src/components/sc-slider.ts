import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

@customElement("sc-slider")
export class ScSlider extends LitElement {
  @property({ type: Number }) value = 50;
  @property({ type: Number }) min = 0;
  @property({ type: Number }) max = 100;
  @property({ type: Number }) step = 1;
  @property({ type: String }) label = "";
  @property({ type: Boolean }) disabled = false;
  @property({ type: Boolean }) showValue = true;

  @state() private _active = false;

  @state() private _sliderId = `sc-slider-${Math.random().toString(36).slice(2, 11)}`;

  static override styles = css`
    :host {
      display: block;
    }

    .wrapper {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xs);
    }

    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: var(--sc-space-sm);
    }

    .label {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
      font-weight: var(--sc-weight-medium);
    }

    .value {
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-weight: var(--sc-weight-medium);
      font-variant-numeric: tabular-nums;
    }

    .track-wrap {
      position: relative;
      height: 20px;
      display: flex;
      align-items: center;
      cursor: pointer;
    }

    .track-wrap[aria-disabled="true"] {
      opacity: var(--sc-opacity-disabled);
      cursor: not-allowed;
    }

    .track {
      flex: 1;
      height: 4px;
      background: var(--sc-border-subtle);
      border-radius: var(--sc-radius-full);
      position: relative;
      overflow: visible;
    }

    .track-fill {
      position: absolute;
      left: 0;
      top: 0;
      bottom: 0;
      background: var(--sc-accent);
      border-radius: var(--sc-radius-full);
      transition: width var(--sc-duration-fast) var(--sc-ease-out);
      min-width: 0;
    }

    .thumb {
      position: absolute;
      top: 50%;
      left: var(--thumb-percent, 0%);
      width: 20px;
      height: 20px;
      margin-left: -10px;
      margin-top: -10px;
      background: var(--sc-bg-surface);
      border-radius: var(--sc-radius-full);
      box-shadow: var(--sc-shadow-md);
      border: 2px solid transparent;
      transform: scale(1);
      transition:
        left var(--sc-duration-fast) var(--sc-ease-out),
        transform var(--sc-duration-fast) var(--sc-ease-out),
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        box-shadow var(--sc-duration-fast) var(--sc-ease-out);
      pointer-events: none;
    }

    .track-wrap:hover:not([aria-disabled="true"]) .thumb {
      transform: scale(1.1);
    }

    .track-wrap.active .thumb {
      transform: scale(0.95);
    }

    input:focus-visible ~ .track .thumb {
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 var(--sc-focus-ring-width) var(--sc-focus-ring);
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
      new CustomEvent("sc-change", {
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
        new CustomEvent("sc-change", {
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
    "sc-slider": ScSlider;
  }
}
