import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("sc-message-group")
export class ScMessageGroup extends LitElement {
  @property({ type: String }) role: "user" | "assistant" = "assistant";

  static override styles = css`
    :host {
      display: block;
      margin-bottom: var(--sc-space-lg, 16px);
    }

    .group {
      display: flex;
      flex-direction: column;
    }

    .group.user {
      align-items: flex-end;
    }

    .group.assistant {
      align-items: flex-start;
    }

    .messages {
      display: flex;
      flex-direction: column;
      gap: 2px;
    }

    .group-footer {
      display: flex;
      align-items: center;
      gap: var(--sc-space-xs);
      margin-top: var(--sc-space-2xs);
      opacity: 0;
      transition: opacity var(--sc-duration-fast) var(--sc-ease-out);
      font-size: var(--sc-text-2xs, 10px);
      color: var(--sc-text-faint);
    }

    .group:hover .group-footer {
      opacity: 1;
    }

    .group.user .group-footer {
      justify-content: flex-end;
    }

    .group.assistant .group-footer {
      justify-content: flex-start;
    }

    .group.user .group-footer slot[name="avatar"] {
      display: none;
    }

    ::slotted([slot="avatar"]) {
      width: var(--sc-space-md);
      height: var(--sc-space-md);
    }

    @media (prefers-reduced-motion: reduce) {
      .group-footer {
        transition: none;
      }
    }
  `;

  override render() {
    return html`
      <div
        class="group ${this.role}"
        role="group"
        aria-label="${this.role === "user" ? "Your messages" : "Assistant messages"}"
      >
        <div class="messages">
          <slot></slot>
        </div>
        <div class="group-footer">
          <slot name="avatar"></slot>
          <slot name="timestamp"></slot>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-message-group": ScMessageGroup;
  }
}
