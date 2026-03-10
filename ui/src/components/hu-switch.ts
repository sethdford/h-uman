import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

@customElement("hu-switch")
export class ScSwitch extends LitElement {
  @property({ type: Boolean }) checked = false;
  @property({ type: Boolean }) disabled = false;
  @property({ type: String }) label = "";
  @state() private _hasToggled = false;

  static override styles = css`
    :host {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-sm);
    }

    .switch {
      position: relative;
      flex-shrink: 0;
      width: 2.25rem;
      height: 1.25rem;
      background: var(--hu-border);
      border-radius: var(--hu-radius-full);
      cursor: pointer;
      transition: background-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .switch.checked {
      background: var(--hu-accent);
    }

    .switch:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .switch:disabled {
      opacity: var(--hu-opacity-disabled);
      cursor: not-allowed;
    }

    @media (prefers-reduced-motion: reduce) {
      .switch {
        transition: none;
      }
    }

    .thumb {
      position: absolute;
      top: var(--hu-space-2xs);
      left: var(--hu-space-2xs);
      width: 1rem;
      height: 1rem;
      background: var(--hu-bg-overlay);
      border-radius: var(--hu-radius-full);
      box-shadow: var(--hu-shadow-sm);
      transition: transform var(--hu-duration-fast) var(--hu-spring-out);
    }

    .switch.checked .thumb {
      transform: translateX(1rem);
    }

    .switch.animated .thumb {
      transition: none;
      animation: hu-thumb-slide-off var(--hu-duration-normal) var(--hu-ease-out) forwards;
    }

    .switch.animated.checked .thumb {
      animation: hu-thumb-slide-on var(--hu-duration-normal) var(--hu-ease-out) forwards;
    }

    @media (prefers-reduced-motion: reduce) {
      .switch.animated .thumb {
        animation: none;
      }
      .switch.animated.checked .thumb {
        transform: translateX(1rem);
      }
    }

    .label {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
      font-family: var(--hu-font);
      font-weight: var(--hu-weight-medium);
    }

    input {
      position: absolute;
      width: 1px;
      height: 1px;
      padding: 0;
      margin: -1px;
      overflow: hidden;
      clip: rect(0, 0, 0, 0);
      white-space: nowrap;
      border: 0;
    }
  `;

  private _onClick(): void {
    if (this.disabled) return;
    this._hasToggled = true;
    this.checked = !this.checked;
    this.dispatchEvent(
      new CustomEvent("hu-change", {
        bubbles: true,
        composed: true,
        detail: { checked: this.checked },
      }),
    );
  }

  private _onKeyDown(e: KeyboardEvent): void {
    if (e.key === " " || e.key === "Enter") {
      e.preventDefault();
      this._onClick();
    }
  }

  override render() {
    return html`
      <div
        class="switch ${this.checked ? "checked" : ""} ${this._hasToggled ? "animated" : ""}"
        role="switch"
        aria-checked=${this.checked}
        aria-label=${this.label || "Toggle"}
        aria-disabled=${this.disabled}
        tabindex=${this.disabled ? -1 : 0}
        @click=${this._onClick}
        @keydown=${this._onKeyDown}
      >
        <span class="thumb" aria-hidden="true"></span>
      </div>
      ${this.label ? html`<span class="label">${this.label}</span>` : null}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-switch": ScSwitch;
  }
}
