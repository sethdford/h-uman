import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";
import { formatRelative } from "../utils.js";

export interface ChatSession {
  id: string;
  title: string;
  ts: number;
  active: boolean;
}

@customElement("sc-chat-sessions-panel")
export class ScChatSessionsPanel extends LitElement {
  @property({ type: Array }) sessions: ChatSession[] = [];

  @property({ type: Boolean, reflect: true }) open = false;

  static override styles = css`
    :host {
      display: block;
      position: absolute;
      left: 0;
      top: 0;
      bottom: 0;
      width: 0;
      overflow: hidden;
      z-index: 10;
      transition: width var(--sc-duration-normal) var(--sc-ease-out);
    }

    :host([open]) {
      width: 280px;
    }

    .panel {
      width: 280px;
      height: 100%;
      display: flex;
      flex-direction: column;
      background: color-mix(
        in srgb,
        var(--sc-bg-surface) var(--sc-glass-standard-bg-opacity, 92%),
        transparent
      );
      backdrop-filter: blur(var(--sc-glass-standard-blur, 24px))
        saturate(var(--sc-glass-standard-saturate, 180%));
      -webkit-backdrop-filter: blur(var(--sc-glass-standard-blur, 24px))
        saturate(var(--sc-glass-standard-saturate, 180%));
      border-right: 1px solid var(--sc-border-subtle);
      box-shadow: var(--sc-shadow-sm);
    }

    .new-chat-btn {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      width: 100%;
      padding: var(--sc-space-md);
      margin: var(--sc-space-sm);
      background: transparent;
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      cursor: pointer;
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        border-color var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out);
    }

    .new-chat-btn:hover {
      background: var(--sc-bg-elevated);
      border-color: var(--sc-accent);
      color: var(--sc-accent-text, var(--sc-accent));
    }

    .new-chat-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .new-chat-btn svg {
      width: 18px;
      height: 18px;
      flex-shrink: 0;
    }

    .session-list {
      flex: 1;
      overflow-y: auto;
      padding: 0 var(--sc-space-sm) var(--sc-space-md);
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .session-item {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      border-radius: var(--sc-radius);
      border-left: 3px solid transparent;
      cursor: pointer;
      transition:
        background var(--sc-duration-fast) var(--sc-ease-out),
        border-color var(--sc-duration-fast) var(--sc-ease-out);
      text-align: left;
      background: transparent;
      border-right: none;
      border-top: none;
      border-bottom: none;
      width: 100%;
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      color: var(--sc-text);
    }

    .session-item:hover {
      background: var(--sc-bg-elevated);
    }

    .session-item.active {
      border-left-color: var(--sc-accent);
      background: color-mix(in srgb, var(--sc-accent) 8%, var(--sc-bg-surface));
    }

    .session-item:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }

    .session-content {
      flex: 1;
      min-width: 0;
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-2xs);
    }

    .session-title {
      font-weight: var(--sc-weight-medium);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .session-ts {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    .delete-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: 24px;
      height: 24px;
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      opacity: 0;
      flex-shrink: 0;
      transition:
        opacity var(--sc-duration-fast) var(--sc-ease-out),
        color var(--sc-duration-fast) var(--sc-ease-out),
        background var(--sc-duration-fast) var(--sc-ease-out);
    }

    .session-item:hover .delete-btn {
      opacity: 1;
    }

    .delete-btn:hover {
      color: var(--sc-error);
      background: var(--sc-error-dim);
    }

    .delete-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
      opacity: 1;
    }

    .delete-btn svg {
      width: 14px;
      height: 14px;
    }

    @media (prefers-reduced-motion: reduce) {
      :host {
        transition: none;
      }
      .new-chat-btn,
      .session-item,
      .delete-btn {
        transition: none;
      }
    }
  `;

  private _onNewChat(): void {
    this.dispatchEvent(
      new CustomEvent("sc-session-new", {
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onSelect(id: string): void {
    this.dispatchEvent(
      new CustomEvent("sc-session-select", {
        bubbles: true,
        composed: true,
        detail: { id },
      }),
    );
  }

  private _onDelete(e: Event, id: string): void {
    e.stopPropagation();
    this.dispatchEvent(
      new CustomEvent("sc-session-delete", {
        bubbles: true,
        composed: true,
        detail: { id },
      }),
    );
  }

  override render() {
    return html`
      <div class="panel" role="navigation" aria-label="Chat sessions">
        <button type="button" class="new-chat-btn" @click=${this._onNewChat} aria-label="New chat">
          ${icons["file-text"]} New Chat
        </button>
        <div class="session-list">
          ${this.sessions.map(
            (s) => html`
              <div
                class="session-item ${s.active ? "active" : ""}"
                role="button"
                tabindex="0"
                @click=${() => this._onSelect(s.id)}
                @keydown=${(e: KeyboardEvent) => {
                  if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    this._onSelect(s.id);
                  }
                }}
              >
                <div class="session-content">
                  <span class="session-title">${s.title || "Untitled"}</span>
                  <span class="session-ts">${formatRelative(s.ts)}</span>
                </div>
                <button
                  type="button"
                  class="delete-btn"
                  aria-label="Delete session"
                  @click=${(e: Event) => this._onDelete(e, s.id)}
                >
                  ${icons.x}
                </button>
              </div>
            `,
          )}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-chat-sessions-panel": ScChatSessionsPanel;
  }
}
