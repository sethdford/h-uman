import { LitElement, html, css } from "lit";
import { customElement, property, query } from "lit/decorators.js";
import type { ChatItem } from "../controllers/chat-controller.js";
import { icons } from "../icons.js";
import "./sc-message-thread.js";
import "./sc-empty-state.js";

@customElement("sc-voice-conversation")
export class ScVoiceConversation extends LitElement {
  @property({ type: Array }) items: ChatItem[] = [];
  @property({ type: Boolean }) isWaiting = false;

  @query("sc-message-thread") private _messageThread!: HTMLElement & {
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
      padding: var(--sc-space-lg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg);
      background: var(--sc-bg-surface);
      background-image: var(--sc-surface-gradient);
      box-shadow: var(--sc-shadow-card);
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
          <sc-empty-state
            .icon=${icons.mic}
            heading="No conversation yet"
            description="Your voice conversation will appear here. Tap the microphone or type a message to begin."
          ></sc-empty-state>
        </div>
      `;
    }

    return html`
      <div class="conversation conversation-thread">
        <sc-message-thread
          .items=${this.items}
          .isWaiting=${this.isWaiting}
          .streamElapsed=${""}
        ></sc-message-thread>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-voice-conversation": ScVoiceConversation;
  }
}
