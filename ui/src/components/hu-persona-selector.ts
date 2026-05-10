import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

export interface PersonaOption {
  id: string;
  name: string;
  description?: string;
}

@customElement("hu-persona-selector")
export class HuPersonaSelector extends LitElement {
  @property({ type: String }) value = "";
  @property({ type: Array }) personas: PersonaOption[] = [];

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
      gap: 0.25rem;
      padding: 0.25rem 0.5rem;
      background: transparent;
      border: none;
      border-radius: var(--hu-radius);
      font-size: var(--hu-text-sm);
      font-family: var(--hu-font);
      color: var(--hu-text-faint);
      cursor: pointer;
      transition:
        color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .trigger:hover {
      color: var(--hu-text-secondary);
      background: var(--hu-hover-overlay);
    }

    .trigger:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-accent);
      outline-offset: calc(-1 * var(--hu-focus-ring-width));
    }

    .trigger .icon {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
    }

    .trigger .chevron {
      width: 0.625rem;
      height: 0.625rem;
      transition: transform var(--hu-duration-fast) var(--hu-ease-out);
    }

    :host([open]) .trigger .chevron {
      transform: rotate(180deg);
    }

    .dropdown {
      position: absolute;
      bottom: calc(100% + var(--hu-space-xs));
      left: 0;
      min-width: 12rem;
      max-height: 16rem;
      overflow-y: auto;
      background: var(--hu-surface-container-high);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      box-shadow: var(--hu-shadow-lg);
      padding: var(--hu-space-xs);
      z-index: 50;
      animation: hu-fade-in var(--hu-duration-fast) var(--hu-ease-out);
    }

    @keyframes hu-fade-in {
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
      flex-direction: column;
      gap: var(--hu-space-3xs);
      width: 100%;
      padding: var(--hu-space-sm) var(--hu-space-md);
      background: none;
      border: none;
      border-radius: var(--hu-radius-sm);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      cursor: pointer;
      text-align: left;
      transition: background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .option:hover,
    .option.focused {
      background: var(--hu-hover-overlay);
    }

    .option[aria-selected="true"] {
      color: var(--hu-accent-text, var(--hu-accent));
    }

    .option-desc {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-tertiary);
    }
  `;

  override render() {
    const selected = this.personas.find((p) => p.id === this.value);
    const label = selected?.name ?? (this.value || "Default");

    return html`
      <button
        class="trigger"
        @click=${this._toggle}
        @keydown=${this._onTriggerKeyDown}
        aria-haspopup="listbox"
        aria-expanded=${this._open}
        aria-label="Select persona style"
      >
        <span class="icon">${icons.user}</span>
        <span>${label}</span>
        <span class="chevron">${icons.chevron}</span>
      </button>
      ${this._open
        ? html`
            <div class="dropdown" role="listbox" aria-label="Persona styles">
              ${this.personas.map(
                (p, i) => html`
                  <button
                    class="option ${i === this._focusedIndex ? "focused" : ""}"
                    role="option"
                    aria-selected=${p.id === this.value}
                    @click=${() => this._select(p.id)}
                  >
                    <span>${p.name}</span>
                    ${p.description
                      ? html`<span class="option-desc">${p.description}</span>`
                      : nothing}
                  </button>
                `,
              )}
            </div>
          `
        : nothing}
    `;
  }

  private _toggle(): void {
    this._open = !this._open;
    this._focusedIndex = -1;
    if (this._open) {
      this._addOutsideClickListener();
    } else {
      this._removeOutsideClickListener();
    }
  }

  private _select(id: string): void {
    this._open = false;
    this._removeOutsideClickListener();
    this.dispatchEvent(
      new CustomEvent("hu-persona-change", {
        bubbles: true,
        composed: true,
        detail: { persona: id },
      }),
    );
  }

  private _onTriggerKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      this._open = false;
      this._removeOutsideClickListener();
    } else if (e.key === "ArrowDown" || e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      if (!this._open) {
        this._open = true;
        this._focusedIndex = 0;
        this._addOutsideClickListener();
      } else {
        this._focusedIndex = Math.min(this._focusedIndex + 1, this.personas.length - 1);
      }
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      this._focusedIndex = Math.max(this._focusedIndex - 1, 0);
    }
    if (this._open && e.key === "Enter" && this._focusedIndex >= 0) {
      this._select(this.personas[this._focusedIndex].id);
    }
  }

  private _outsideHandler = (e: MouseEvent) => {
    if (!this.contains(e.target as Node)) {
      this._open = false;
      this._removeOutsideClickListener();
    }
  };

  private _addOutsideClickListener(): void {
    document.addEventListener("click", this._outsideHandler, true);
  }

  private _removeOutsideClickListener(): void {
    document.removeEventListener("click", this._outsideHandler, true);
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this._removeOutsideClickListener();
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-persona-selector": HuPersonaSelector;
  }
}
