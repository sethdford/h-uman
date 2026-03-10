import { LitElement, html, css } from "lit";
import { customElement, property, query } from "lit/decorators.js";
import type { ChatItem } from "../controllers/chat-controller.js";
import { icons } from "../icons.js";
import "./hu-message-thread.js";
import "./hu-empty-state.js";

@customElement("hu-voice-conversation")
export class ScVoiceConversation extends LitElement {
  @property({ type: Array }) items: ChatItem[] = [];
  @property({ type: Boolean }) isWaiting = false;

  @query("hu-message-thread") private _messageThread!: HTMLElement & {
    scrollToBottom: () => void;
  };

  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      flex: 1;
      min-height: 0;
    }

    .conversation {
      display: flex;
      flex-direction: column;
      flex: 1;
      min-height: 0;
      padding: var(--hu-space-lg);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-lg);
      background: var(--hu-bg-surface);
      background-image: var(--hu-surface-gradient);
      box-shadow: var(--hu-shadow-card);
    }

    .conversation-empty {
      overflow-y: auto;
      scroll-behavior: smooth;
    }

    .conversation-thread {
      overflow-y: auto;
      scroll-behavior: smooth;
      padding: 0;
    }
  `;

  scrollToBottom(): void {
    this._messageThread?.scrollToBottom();
  }

  override render() {
    const showEmpty = this.items.length === 0 && !this.isWaiting;

    if (showEmpty) {
      return html`
        <div class="conversation conversation-empty">
          <hu-empty-state
            .icon=${icons.mic}
            heading="No conversation yet"
            description="Your voice conversation will appear here. Tap the microphone or type a message to begin."
          ></hu-empty-state>
        </div>
      `;
    }

    return html`
      <div class="conversation conversation-thread">
        <hu-message-thread
          .items=${this.items}
          .isWaiting=${this.isWaiting}
          .streamElapsed=${""}
        ></hu-message-thread>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-voice-conversation": ScVoiceConversation;
  }
}
