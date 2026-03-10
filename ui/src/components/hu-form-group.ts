import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

const FORM_CONTROL_SELECTOR = "hu-input, hu-select, hu-combobox, hu-checkbox, hu-textarea";

@customElement("hu-form-group")
export class ScFormGroup extends LitElement {
  @property({ type: String }) title = "";
  @property({ type: String }) description = "";

  @state() private _dirty = false;

  static override styles = css`
    :host {
      display: block;
    }

    .wrapper {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
      padding: var(--hu-space-md);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      background: var(--hu-bg-surface);
    }

    .header {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-2xs);
    }

    .title {
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      font-family: var(--hu-font);
    }

    .description {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      font-family: var(--hu-font);
    }

    .dirty-indicator {
      font-size: var(--hu-text-xs);
      color: var(--hu-accent);
      font-family: var(--hu-font);
    }

    .content {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-sm);
    }
  `;

  get dirty(): boolean {
    return this._dirty;
  }

  get valid(): boolean {
    const controls = this.querySelectorAll<HTMLElement & { error?: string }>(FORM_CONTROL_SELECTOR);
    for (const c of controls) {
      if (c.error && c.error.trim() !== "") return false;
    }
    return true;
  }

  validate(): boolean {
    return this.valid;
  }

  reset(): void {
    const controls = this.querySelectorAll<HTMLElement & { error?: string }>(FORM_CONTROL_SELECTOR);
    for (const c of controls) {
      c.error = "";
    }
    this._dirty = false;
  }

  private _onFormControlChange = (): void => {
    this._dirty = true;
  };

  private _onSubmit(e: SubmitEvent): void {
    e.preventDefault();
    this.dispatchEvent(
      new CustomEvent("hu-form-submit", {
        bubbles: true,
        composed: true,
        detail: {} as Record<string, unknown>,
      }),
    );
  }

  override connectedCallback(): void {
    super.connectedCallback();
    this.addEventListener("hu-input", this._onFormControlChange);
    this.addEventListener("hu-change", this._onFormControlChange);
    this.addEventListener("hu-combobox-change", this._onFormControlChange);
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this.removeEventListener("hu-input", this._onFormControlChange);
    this.removeEventListener("hu-change", this._onFormControlChange);
    this.removeEventListener("hu-combobox-change", this._onFormControlChange);
  }

  override render() {
    return html`
      <form class="wrapper" @submit=${this._onSubmit}>
        ${this.title || this.description || this._dirty
          ? html`
              <div class="header">
                ${this.title ? html`<h3 class="title">${this.title}</h3>` : null}
                ${this.description ? html`<p class="description">${this.description}</p>` : null}
                ${this._dirty ? html`<span class="dirty-indicator">Unsaved changes</span>` : null}
              </div>
            `
          : null}
        <div class="content">
          <slot></slot>
        </div>
      </form>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-form-group": ScFormGroup;
  }
}
