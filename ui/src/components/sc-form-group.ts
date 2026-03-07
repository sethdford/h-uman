import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

const FORM_CONTROL_SELECTOR = "sc-input, sc-select, sc-combobox, sc-checkbox, sc-textarea";

@customElement("sc-form-group")
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
      gap: var(--sc-space-md);
      padding: var(--sc-space-md);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      background: var(--sc-bg-surface);
    }

    .header {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .title {
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold);
      color: var(--sc-text);
      font-family: var(--sc-font);
    }

    .description {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
    }

    .dirty-indicator {
      font-size: var(--sc-text-xs);
      color: var(--sc-accent);
      font-family: var(--sc-font);
    }

    .content {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-sm);
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
      new CustomEvent("sc-form-submit", {
        bubbles: true,
        composed: true,
        detail: {} as Record<string, unknown>,
      }),
    );
  }

  override connectedCallback(): void {
    super.connectedCallback();
    this.addEventListener("sc-input", this._onFormControlChange);
    this.addEventListener("sc-change", this._onFormControlChange);
    this.addEventListener("sc-combobox-change", this._onFormControlChange);
  }

  override disconnectedCallback(): void {
    super.disconnectedCallback();
    this.removeEventListener("sc-input", this._onFormControlChange);
    this.removeEventListener("sc-change", this._onFormControlChange);
    this.removeEventListener("sc-combobox-change", this._onFormControlChange);
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
    "sc-form-group": ScFormGroup;
  }
}
