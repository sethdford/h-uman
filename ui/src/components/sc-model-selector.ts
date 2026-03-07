import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

@customElement("sc-model-selector")
export class ScModelSelector extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: Array }) models: Array<{ id: string; name: string; provider?: string }> = [];

  @state() private _open = false;
  @state() private _focusedIndex = -1;

  static override styles = css`
    :host {
      display: inline-block;
      position: relative;
    }

    .trigger {
      display: inline-flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      padding: var(--sc-space-2xs) var(--sc-space-sm);
      background: var(--sc-bg-elevated);
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-full);
      font-size: var(--sc-text-xs);
      font-family: var(--sc-font);
      color: var(--sc-text-muted);
      cursor: pointer;
      transition:
        color var(--sc-duration-fast),
        border-color var(--sc-duration-fast);
    }

    .trigger:hover {
      color: var(--sc-text);
      border-color: var(--sc-border);
    }

    .trigger:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .trigger svg {
      width: 12px;
      height: 12px;
    }

    .dropdown {
      position: absolute;
      bottom: 100%;
      left: 0;
      margin-bottom: var(--sc-space-xs);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-lg);
      max-height: 240px;
      overflow-y: auto;
      min-width: 180px;
      z-index: 20;
      animation: sc-dropdown-enter var(--sc-duration-fast) var(--sc-ease-out);
    }

    @keyframes sc-dropdown-enter {
      from {
        opacity: 0;
        transform: translateY(4px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    .option {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: var(--sc-space-sm) var(--sc-space-md);
      font-size: var(--sc-text-sm);
      font-family: var(--sc-font);
      color: var(--sc-text);
      cursor: pointer;
      transition: background var(--sc-duration-fast);
      border: none;
      background: transparent;
      width: 100%;
      text-align: left;
    }

    .option:hover,
    .option.focused {
      background: var(--sc-bg-elevated);
    }

    .option.selected {
      color: var(--sc-accent);
    }

    .option .check {
      width: 14px;
      height: 14px;
      display: flex;
    }

    .option .check svg {
      width: 100%;
      height: 100%;
    }

    .provider {
      font-size: var(--sc-text-2xs, 10px);
      color: var(--sc-text-faint);
      margin-left: var(--sc-space-xs);
    }

    @media (prefers-reduced-motion: reduce) {
      .dropdown {
        animation: none;
      }
    }
  `;

  private _onTriggerClick(e: Event) {
    e.stopPropagation();
    this._open = !this._open;
    this._focusedIndex = -1;
  }

  private _onSelect(id: string) {
    this._open = false;
    this.dispatchEvent(
      new CustomEvent("sc-model-change", {
        bubbles: true,
        composed: true,
        detail: { model: id },
      }),
    );
  }

  private _onKeydown(e: KeyboardEvent) {
    if (!this._open) {
      if (e.key === "Enter" || e.key === " " || e.key === "ArrowDown") {
        e.preventDefault();
        this._open = true;
        this._focusedIndex = 0;
      }
      return;
    }
    if (e.key === "Escape") {
      this._open = false;
    } else if (e.key === "ArrowDown") {
      e.preventDefault();
      this._focusedIndex = Math.min(this._focusedIndex + 1, this.models.length - 1);
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      this._focusedIndex = Math.max(this._focusedIndex - 1, 0);
    } else if (e.key === "Enter" && this._focusedIndex >= 0) {
      this._onSelect(this.models[this._focusedIndex].id);
    }
  }

  private _onClickOutside = (e: MouseEvent) => {
    if (!this.contains(e.target as Node)) {
      this._open = false;
    }
  };

  override connectedCallback() {
    super.connectedCallback();
    document.addEventListener("click", this._onClickOutside);
  }

  override disconnectedCallback() {
    document.removeEventListener("click", this._onClickOutside);
    super.disconnectedCallback();
  }

  override render() {
    const selected = this.models.find((m) => m.id === this.value);
    const label = (selected?.name ?? this.value) || "Select model";

    return html`
      <button
        class="trigger"
        role="combobox"
        aria-expanded=${this._open}
        aria-haspopup="listbox"
        @click=${this._onTriggerClick}
        @keydown=${this._onKeydown}
      >
        ${label} ${icons["caret-down"]}
      </button>
      ${this._open
        ? html`
            <div class="dropdown" role="listbox">
              ${this.models.map(
                (m, i) => html`
                  <button
                    class="option ${m.id === this.value ? "selected" : ""} ${i ===
                    this._focusedIndex
                      ? "focused"
                      : ""}"
                    role="option"
                    aria-selected=${m.id === this.value}
                    @click=${() => this._onSelect(m.id)}
                  >
                    <span
                      >${m.name}${m.provider
                        ? html`<span class="provider">${m.provider}</span>`
                        : nothing}</span
                    >
                    ${m.id === this.value
                      ? html`<span class="check">${icons.check}</span>`
                      : nothing}
                  </button>
                `,
              )}
            </div>
          `
        : nothing}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-model-selector": ScModelSelector;
  }
}
