import { LitElement } from "lit";
import { customElement } from "lit/decorators.js";

@customElement("sc-sessions-view")
export class ScSessionsView extends LitElement {
  override connectedCallback(): void {
    super.connectedCallback();
    this.dispatchEvent(
      new CustomEvent("navigate", {
        detail: "chat",
        bubbles: true,
        composed: true,
      }),
    );
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-sessions-view": ScSessionsView;
  }
}
