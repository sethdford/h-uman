import { LitElement, html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";

export interface ToastOptions {
  message: string;
  variant?: "info" | "success" | "warning" | "error";
  duration?: number;
  action?: { label: string; callback?: () => void };
}

interface ToastItem extends ToastOptions {
  id: string;
  visible: boolean;
}

@customElement("hu-toast")
export class ScToast extends LitElement {
  static override styles = css`
    :host {
      position: fixed;
      bottom: var(--hu-space-lg);
      right: var(--hu-space-lg);
      z-index: 10000;
      display: flex;
      flex-direction: column-reverse;
      gap: var(--hu-space-sm);
      max-width: min(400px, calc(100vw - var(--hu-space-xl)));
      pointer-events: none;
    }

    @media (max-width: 640px) /* --hu-breakpoint-md */ {
      :host {
        left: 50%;
        right: auto;
        transform: translateX(-50%);
        bottom: var(--hu-space-md);
      }
    }

    .toast {
      display: flex;
      align-items: flex-start;
      gap: var(--hu-space-md);
      padding: var(--hu-space-md) var(--hu-space-lg);
      background: var(--hu-bg-overlay);
      border-radius: var(--hu-radius-lg);
      box-shadow: var(--hu-shadow-lg);
      border: 1px solid var(--hu-border);
      pointer-events: auto;
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      animation: hu-overshoot-in var(--hu-duration-normal) var(--hu-ease-out);
      transition:
        transform var(--hu-duration-normal) var(--hu-ease-out),
        opacity var(--hu-duration-normal);
    }

    .toast.exiting {
      animation: hu-slide-out-down var(--hu-duration-fast) var(--hu-ease-in) forwards;
    }

    @media (prefers-reduced-motion: reduce) {
      .toast {
        transition: none;
      }
      .toast.exiting {
        animation: hu-fade-out var(--hu-duration-instant) forwards;
      }
    }

    .toast.variant-info {
      border-left: 4px solid var(--hu-info);
    }

    .toast.variant-success {
      border-left: 4px solid var(--hu-success);
    }

    .toast.variant-warning {
      border-left: 4px solid var(--hu-warning);
    }

    .toast.variant-error {
      border-left: 4px solid var(--hu-error);
    }

    .message {
      flex: 1;
      color: var(--hu-text);
      line-height: var(--hu-leading-normal);
    }

    .actions {
      display: flex;
      align-items: center;
      gap: var(--hu-space-sm);
      flex-shrink: 0;
    }

    .action-btn {
      padding: var(--hu-space-xs) var(--hu-space-sm);
      background: transparent;
      border: none;
      border-radius: var(--hu-radius-sm);
      color: var(--hu-accent-text, var(--hu-accent));
      font-family: var(--hu-font);
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-medium);
      cursor: pointer;
      transition: color var(--hu-duration-fast);
    }

    .action-btn:hover {
      color: var(--hu-accent-hover, var(--hu-accent));
    }

    .dismiss-btn {
      padding: var(--hu-space-xs);
      background: transparent;
      border: none;
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text-muted);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      transition: color var(--hu-duration-fast);
      line-height: 1;
    }

    .dismiss-btn:hover {
      color: var(--hu-text);
    }

    .dismiss-btn svg {
      width: var(--hu-icon-sm);
      height: var(--hu-icon-sm);
    }
  `;

  @state() private _toasts: ToastItem[] = [];
  private _counter = 0;
  private _timeouts = new Map<string, ReturnType<typeof setTimeout>>();

  /** Show a toast. Appends singleton container to body if needed. */
  static show(options: ToastOptions): void {
    let container = document.querySelector<ScToast>("hu-toast");
    if (!container) {
      container = document.createElement("hu-toast");
      document.body.appendChild(container);
    }
    container._addToast(options);
  }

  private _addToast(options: ToastOptions): void {
    const id = `toast-${++this._counter}`;
    const variant = options.variant ?? "info";
    const duration = options.duration ?? 4000;
    const item: ToastItem = {
      ...options,
      id,
      variant,
      duration,
      visible: true,
    };
    this._toasts = [...this._toasts.slice(-2), item];
    this.requestUpdate();
    if (duration > 0) {
      const t = setTimeout(() => this._dismiss(id), duration);
      this._timeouts.set(id, t);
    }
  }

  private _dismiss(id: string): void {
    const t = this._timeouts.get(id);
    if (t) {
      clearTimeout(t);
      this._timeouts.delete(id);
    }
    this._toasts = this._toasts.map((item) =>
      item.id === id ? { ...item, visible: false } : item,
    );
    this.requestUpdate();
    setTimeout(() => {
      this._toasts = this._toasts.filter((item) => item.id !== id);
      this.requestUpdate();
    }, 200);
  }

  private _onActionClick(item: ToastItem): void {
    item.action?.callback?.();
    this._dismiss(item.id);
  }

  override render() {
    if (this._toasts.length === 0) return nothing;

    return html`
      ${this._toasts.map(
        (item) => html`
          <div
            class="toast hu-entry-slide-up variant-${item.variant} ${item.visible ? "" : "exiting"}"
            role="status"
            aria-live="polite"
            aria-atomic="true"
          >
            <span class="message">${item.message}</span>
            <div class="actions">
              ${item.action
                ? html`
                    <button class="action-btn" @click=${() => this._onActionClick(item)}>
                      ${item.action.label}
                    </button>
                  `
                : nothing}
              <button
                class="dismiss-btn"
                aria-label="Dismiss"
                @click=${() => this._dismiss(item.id)}
              >
                <svg viewBox="0 0 256 256" fill="currentColor">
                  <path
                    d="M205.66 194.34a8 8 0 0 0-11.32-11.32L128 116.69 61.66 50.34a8 8 0 0 0-11.32 11.32L116.69 128 50.34 194.34a8 8 0 1 0 11.32 11.32L128 139.31l66.34 66.33a8 8 0 0 0 11.32-11.32Z"
                  />
                </svg>
              </button>
            </div>
          </div>
        `,
      )}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-toast": ScToast;
  }
}
