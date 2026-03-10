import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

@customElement("hu-message-group")
export class ScMessageGroup extends LitElement {
  @property({ type: String }) role: "user" | "assistant" = "assistant";

  static override styles = css`
    :host {
      display: block;
      margin-bottom: var(--hu-space-lg, 1rem);
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

    .group-inner {
      display: flex;
      flex-direction: row;
      align-items: flex-start;
      gap: var(--hu-space-sm);
      max-width: 100%;
    }

    .group.user .group-inner {
      flex-direction: row-reverse;
    }

    .avatar {
      flex-shrink: 0;
      width: 1.75rem;
      height: 1.75rem;
      border-radius: 50%;
      display: flex;
      align-items: center;
      justify-content: center;
    }

    .avatar.assistant {
      background: var(--hu-accent);
      color: var(--hu-on-accent);
    }

    .avatar.user {
      background: var(--hu-accent-subtle);
      color: var(--hu-accent);
    }

    .avatar svg {
      width: 0.875rem;
      height: 0.875rem;
    }

    .messages {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
      min-width: 0;
    }

    .group-footer {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
      margin-top: var(--hu-space-2xs);
      opacity: 0;
      transition: opacity var(--hu-duration-fast) var(--hu-ease-out);
      font-size: var(--hu-text-2xs, 0.625rem);
      color: var(--hu-text-faint);
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
        <div class="group-inner">
          <div class="avatar ${this.role}" aria-hidden="true">
            <slot name="avatar"></slot>
          </div>
          <div class="messages">
            <slot></slot>
          </div>
        </div>
        <div class="group-footer">
          <slot name="timestamp"></slot>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-message-group": ScMessageGroup;
  }
}
