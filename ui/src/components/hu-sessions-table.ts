import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import { formatDate } from "../utils.js";
import { icons } from "../icons.js";
import "../components/hu-card.js";
import "../components/hu-empty-state.js";
import "../components/hu-button.js";

export interface SessionItem {
  key?: string;
  label?: string;
  created_at?: number;
  last_active?: number;
  turn_count?: number;
}

@customElement("hu-sessions-table")
export class ScSessionsTable extends LitElement {
  @property({ type: Array }) sessions: SessionItem[] = [];
  @property({ type: String }) emptyHeading = "No conversations yet";
  @property({ type: String }) emptyDescription = "Start your first chat to see h-uman in action.";
  @property({ type: String }) emptyActionLabel = "Start a Conversation";
  @property({ type: String }) emptyActionTarget = "chat";

  static override styles = css`
    :host {
      display: block;
    }

    .sessions-table {
      width: 100%;
      border-collapse: collapse;
    }

    .sessions-table th,
    .sessions-table td {
      padding: var(--hu-space-sm) var(--hu-space-md);
      text-align: left;
      border-bottom: 1px solid var(--hu-border);
      font-size: var(--hu-text-sm);
    }

    .sessions-table th {
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text-muted);
    }

    .sessions-table tr:last-child td {
      border-bottom: none;
    }

    .session-row {
      cursor: pointer;
      transition: background-color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .session-row:hover {
      background-color: var(--hu-hover-overlay);
    }

    .session-row:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: -2px;
    }

    @media (prefers-reduced-motion: reduce) {
      .session-row {
        transition: none;
      }
    }
  `;

  private _onSessionClick(session: SessionItem): void {
    const key = session.key ?? "";
    this.dispatchEvent(
      new CustomEvent("session-select", {
        detail: { key },
        bubbles: true,
        composed: true,
      }),
    );
  }

  override render() {
    if (this.sessions.length === 0) {
      return html`
        <hu-empty-state
          .icon=${icons["chat-circle"]}
          heading=${this.emptyHeading}
          description=${this.emptyDescription}
        >
          <hu-button
            variant="primary"
            @click=${() =>
              this.dispatchEvent(
                new CustomEvent("navigate", {
                  detail: this.emptyActionTarget,
                  bubbles: true,
                  composed: true,
                }),
              )}
          >
            ${this.emptyActionLabel}
          </hu-button>
        </hu-empty-state>
      `;
    }

    return html`
      <table class="sessions-table">
        <thead>
          <tr>
            <th>Session</th>
            <th>Turns</th>
            <th>Last active</th>
          </tr>
        </thead>
        <tbody>
          ${this.sessions.map(
            (s) => html`
              <tr
                class="session-row"
                role="button"
                tabindex="0"
                aria-label=${`Open session ${s.label ?? s.key ?? "unnamed"}`}
                @click=${() => this._onSessionClick(s)}
                @keydown=${(e: KeyboardEvent) => {
                  if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    this._onSessionClick(s);
                  }
                }}
              >
                <td>${s.label ?? s.key ?? "unnamed"}</td>
                <td>${s.turn_count ?? 0}</td>
                <td>${formatDate(s.last_active)}</td>
              </tr>
            `,
          )}
        </tbody>
      </table>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-sessions-table": ScSessionsTable;
  }
}
