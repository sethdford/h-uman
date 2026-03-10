import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";

@customElement("hu-message-branch")
export class ScMessageBranch extends LitElement {
  @property({ type: Number }) branches = 1;
  @property({ type: Number }) current = 0;

  static override styles = css`
    :host {
      display: inline-block;
    }

    .pill {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      border-radius: var(--hu-radius-full);
      background: color-mix(
        in srgb,
        var(--hu-surface) var(--hu-glass-subtle-bg-opacity, 4%),
        transparent
      );
      backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px))
        saturate(var(--hu-glass-subtle-saturate, 120%));
      -webkit-backdrop-filter: blur(var(--hu-glass-subtle-blur, 12px))
        saturate(var(--hu-glass-subtle-saturate, 120%));
      border: 1px solid
        color-mix(in srgb, var(--hu-border) var(--hu-glass-subtle-border-opacity, 5%), transparent);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
    }

    .btn {
      display: flex;
      align-items: center;
      justify-content: center;
      padding: var(--hu-space-2xs);
      border: none;
      background: transparent;
      color: var(--hu-text-muted);
      cursor: pointer;
      border-radius: var(--hu-radius);
      transition: color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .btn:hover:not(:disabled) {
      color: var(--hu-text);
    }

    .btn:disabled {
      opacity: 0.4;
      cursor: not-allowed;
    }

    .btn:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: 2px;
    }

    .btn svg {
      width: 14px;
      height: 14px;
    }

    .label {
      min-width: 2.5em;
      text-align: center;
    }
  `;

  private _prev() {
    if (this.current > 0) {
      this.current--;
      this._dispatchChange();
    }
  }

  private _next() {
    if (this.current < this.branches - 1) {
      this.current++;
      this._dispatchChange();
    }
  }

  private _dispatchChange() {
    this.dispatchEvent(
      new CustomEvent("branch-change", {
        detail: { branch: this.current },
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onKeyDown(e: KeyboardEvent) {
    if (e.key === "ArrowLeft") {
      e.preventDefault();
      this._prev();
    } else if (e.key === "ArrowRight") {
      e.preventDefault();
      this._next();
    }
  }

  override render() {
    const canPrev = this.current > 0;
    const canNext = this.current < this.branches - 1;
    const displayCurrent = Math.min(this.current + 1, this.branches);

    return html`
      <div
        class="pill"
        role="group"
        tabindex="0"
        aria-label="Branch ${displayCurrent} of ${this.branches}"
        @keydown=${this._onKeyDown}
      >
        <button
          class="btn"
          type="button"
          ?disabled=${!canPrev}
          aria-label="Previous branch"
          @click=${this._prev}
        >
          ${icons.chevron}
        </button>
        <span class="label">${displayCurrent} / ${this.branches}</span>
        <button
          class="btn"
          type="button"
          ?disabled=${!canNext}
          aria-label="Next branch"
          @click=${this._next}
        >
          ${icons["chevron-right"]}
        </button>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-message-branch": ScMessageBranch;
  }
}
