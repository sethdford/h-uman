import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

type GateStatus = "pending" | "approved" | "rejected" | "timed_out";

@customElement("hu-approval-gate")
export class HuApprovalGate extends LitElement {
  @property() gateId = "";
  @property() description = "";
  @property() status: GateStatus = "pending";

  static override styles = css`
    :host {
      display: block;
      font-family: var(--hu-font);
    }

    .gate-card {
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-lg);
      padding: var(--hu-space-lg);
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
    }

    .gate-card.status-pending {
      border-color: color-mix(in srgb, var(--hu-warning) 30%, var(--hu-border));
      background: color-mix(in srgb, var(--hu-warning) 3%, var(--hu-bg-surface));
    }

    .gate-card.status-approved {
      border-color: color-mix(in srgb, var(--hu-success) 30%, var(--hu-border));
      background: color-mix(in srgb, var(--hu-success) 3%, var(--hu-bg-surface));
    }

    .gate-card.status-rejected {
      border-color: color-mix(in srgb, var(--hu-error) 30%, var(--hu-border));
      background: color-mix(in srgb, var(--hu-error) 3%, var(--hu-bg-surface));
    }

    .gate-card.status-timed_out {
      border-color: color-mix(in srgb, var(--hu-error) 30%, var(--hu-border));
      background: color-mix(in srgb, var(--hu-error) 3%, var(--hu-bg-surface));
    }

    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: var(--hu-space-md);
    }

    .title {
      font-size: var(--hu-text-md);
      font-weight: 600;
      color: var(--hu-text);
      margin: 0;
    }

    .status-badge {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: var(--hu-space-xs) var(--hu-space-sm);
      border-radius: var(--hu-radius-full);
      font-size: var(--hu-text-xs);
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }

    .status-badge.pending {
      background: color-mix(in srgb, var(--hu-warning) 12%, var(--hu-bg-surface));
      color: var(--hu-warning);
      border: 1px solid color-mix(in srgb, var(--hu-warning) 25%, var(--hu-border));
    }

    .status-badge.approved {
      background: color-mix(in srgb, var(--hu-success) 12%, var(--hu-bg-surface));
      color: var(--hu-success);
      border: 1px solid color-mix(in srgb, var(--hu-success) 25%, var(--hu-border));
    }

    .status-badge.rejected {
      background: color-mix(in srgb, var(--hu-error) 12%, var(--hu-bg-surface));
      color: var(--hu-error);
      border: 1px solid color-mix(in srgb, var(--hu-error) 25%, var(--hu-border));
    }

    .status-badge.timed_out {
      background: color-mix(in srgb, var(--hu-error) 12%, var(--hu-bg-surface));
      color: var(--hu-error);
      border: 1px solid color-mix(in srgb, var(--hu-error) 25%, var(--hu-border));
    }

    .description {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-secondary);
      margin: 0;
    }

    .actions {
      display: flex;
      gap: var(--hu-space-sm);
      flex-wrap: wrap;
    }

    button {
      padding: var(--hu-space-xs) var(--hu-space-md);
      border-radius: var(--hu-radius-md);
      font-size: var(--hu-text-sm);
      font-weight: 600;
      cursor: pointer;
      transition: all var(--hu-duration-fast) var(--hu-ease-out);
      border: 1px solid transparent;
      font-family: var(--hu-font);
    }

    button:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .btn-approve {
      background: var(--hu-success);
      color: white;
    }

    .btn-approve:hover:not(:disabled) {
      background: color-mix(in srgb, var(--hu-success) 90%, var(--hu-color-black));
    }

    .btn-approve:active:not(:disabled) {
      transform: scale(0.98);
    }

    .btn-reject {
      background: var(--hu-error);
      color: white;
    }

    .btn-reject:hover:not(:disabled) {
      background: color-mix(in srgb, var(--hu-error) 90%, var(--hu-color-black));
    }

    .btn-reject:active:not(:disabled) {
      transform: scale(0.98);
    }

    button:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }

    @media (prefers-reduced-motion: reduce) {
      button {
        transition: none;
      }
      button:active:not(:disabled) {
        transform: none;
      }
    }
  `;

  private _onApprove() {
    this.dispatchEvent(
      new CustomEvent("gate-approve", {
        detail: { gateId: this.gateId },
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onReject() {
    this.dispatchEvent(
      new CustomEvent("gate-reject", {
        detail: { gateId: this.gateId },
        bubbles: true,
        composed: true,
      }),
    );
  }

  override render() {
    const isPending = this.status === "pending";

    return html`
      <div class="gate-card status-${this.status}">
        <div class="header">
          <h3 class="title">Approval Required</h3>
          <span class="status-badge ${this.status}">${this._formatStatus()}</span>
        </div>

        ${this.description ? html`<p class="description">${this.description}</p>` : ""}
        ${
          isPending
            ? html`
                <div class="actions">
                  <button
                    type="button"
                    class="btn-approve"
                    @click=${this._onApprove}
                    aria-label="Approve this gate"
                  >
                    Approve
                  </button>
                  <button
                    type="button"
                    class="btn-reject"
                    @click=${this._onReject}
                    aria-label="Reject this gate"
                  >
                    Reject
                  </button>
                </div>
              `
            : ""
        }
      </div>
    `;
  }

  private _formatStatus(): string {
    const statusMap: Record<GateStatus, string> = {
      pending: "Pending",
      approved: "Approved",
      rejected: "Rejected",
      timed_out: "Timed Out",
    };
    return statusMap[this.status] || "Unknown";
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-approval-gate": HuApprovalGate;
  }
}
