import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

@customElement("hu-memory-event")
export class ScMemoryEvent extends LitElement {
  @property({ type: String }) action: "recall" | "store" | "forget" = "recall";
  @property({ type: String }) key = "";
  @property({ type: String }) value = "";

  @state() private _expanded = false;

  static override styles = css`
    :host {
      display: block;
      contain: layout style;
    }
    .memory-event {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-2xs) var(--hu-space-sm);
      background: color-mix(in srgb, var(--hu-accent-tertiary, var(--hu-accent)) 5%, transparent);
      border-left: 2px solid
        color-mix(in srgb, var(--hu-accent-tertiary, var(--hu-accent)) 25%, transparent);
      border-radius: var(--hu-radius-sm);
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }
    .icon {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
      flex-shrink: 0;
      margin-top: 0.125rem;
      color: var(--hu-accent-tertiary, var(--hu-accent));
    }
    .icon svg {
      width: 100%;
      height: 100%;
    }
    .body {
      flex: 1;
      min-width: 0;
    }
    .summary {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
      cursor: pointer;
      user-select: none;
      transition: color var(--hu-duration-fast) var(--hu-ease-out);
    }
    .summary:hover .label {
      color: var(--hu-text);
    }
    .action {
      font-weight: var(--hu-weight-medium);
      text-transform: capitalize;
    }
    .key {
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-xs);
      padding: 0.0625rem var(--hu-space-xs);
      background: var(--hu-bg-inset);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text);
    }
    .caret {
      display: inline-flex;
      width: 0.75rem;
      height: 0.75rem;
      flex-shrink: 0;
      transition: transform var(--hu-duration-fast) var(--hu-ease-out);
    }
    .caret.open {
      transform: rotate(90deg);
    }
    .caret svg {
      width: 100%;
      height: 100%;
    }
    .detail {
      margin-top: var(--hu-space-xs);
      padding: var(--hu-space-xs) var(--hu-space-sm);
      background: var(--hu-bg-inset);
      border-radius: var(--hu-radius-sm);
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-xs);
      color: var(--hu-text);
      white-space: pre-wrap;
      word-break: break-word;
      max-height: 8rem;
      overflow-y: auto;
    }
    @media (prefers-reduced-motion: reduce) {
      .caret {
        transition: none;
      }
    }
  `;

  private _toggle(): void {
    this._expanded = !this._expanded;
  }

  override render() {
    const actionLabel =
      this.action === "recall" ? "Recalled" : this.action === "store" ? "Stored" : "Forgot";
    const actionIcon = this.action === "forget" ? icons.trash : icons.brain;

    return html`
      <div class="memory-event" role="status" aria-label="Memory ${this.action}">
        <span class="icon" aria-hidden="true">${actionIcon}</span>
        <div class="body">
          <div class="summary" @click=${this._toggle}>
            <span class="label">
              <span class="action">${actionLabel}</span>
              <span class="key">${this.key}</span>
            </span>
            ${
              this.value
                ? html`<span class="caret ${this._expanded ? "open" : ""}"
                    >${icons["caret-right"]}</span
                  >`
                : nothing
            }
          </div>
          ${this._expanded && this.value ? html`<div class="detail">${this.value}</div>` : nothing}
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-memory-event": ScMemoryEvent;
  }
}
