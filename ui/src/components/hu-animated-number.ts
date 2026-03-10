import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

@customElement("hu-animated-number")
export class ScAnimatedNumber extends LitElement {
  @property({ type: Number }) value = 0;
  @property({ type: Number }) duration = 600;
  @property({ type: String }) suffix = "";
  @property({ type: String }) prefix = "";
  @state() private _displayed = 0;
  private _raf = 0;
  private _start = 0;
  private _from = 0;

  static override styles = css`
    :host {
      display: inline;
      font-variant-numeric: tabular-nums;
      font-family: var(--hu-font);
    }
  `;

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("value")) {
      this._animate((changed.get("value") as number) ?? 0, this.value);
    }
  }

  private _animate(from: number, to: number): void {
    if (this._raf) cancelAnimationFrame(this._raf);
    if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
      this._displayed = to;
      return;
    }
    this._from = from;
    this._start = performance.now();
    const tick = (now: number) => {
      const elapsed = now - this._start;
      const t = Math.min(elapsed / this.duration, 1);
      const eased = 1 - Math.pow(1 - t, 3);
      this._displayed = Math.round(this._from + (to - this._from) * eased);
      if (t < 1) {
        this._raf = requestAnimationFrame(tick);
      } else {
        this._displayed = to;
      }
    };
    this._raf = requestAnimationFrame(tick);
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    if (this._raf) cancelAnimationFrame(this._raf);
  }

  override render() {
    return html`<span role="status" aria-live="polite"
      >${this.prefix}${this._displayed}${this.suffix}</span
    >`;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-animated-number": ScAnimatedNumber;
  }
}
