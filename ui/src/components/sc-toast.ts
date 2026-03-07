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

@customElement("sc-toast")
export class ScToast extends LitElement {
  static override styles = css`
    :host {
      position: fixed;
      bottom: var(--sc-space-lg);
      right: var(--sc-space-lg);
      z-index: 10000;
      display: flex;
      flex-direction: column-reverse;
      gap: var(--sc-space-sm);
      max-width: min(400px, calc(100vw - var(--sc-space-xl)));
      pointer-events: none;
    }

    @media (max-width: 640px) /* --sc-breakpoint-md */ {
      :host {
        left: 50%;
        right: auto;
        transform: translateX(-50%);
        bottom: var(--sc-space-md);
      }
    }

    .toast {
      display: flex;
      align-items: flex-start;
      gap: var(--sc-space-md);
      padding: var(--sc-space-md) var(--sc-space-lg);
      background: var(--sc-bg-overlay);
      border-radius: var(--sc-radius-lg);
      box-shadow: var(--sc-shadow-lg);
      border: 1px solid var(--sc-border);
      pointer-events: auto;
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      animation: sc-overshoot-in var(--sc-duration-normal) var(--sc-ease-out);
      transition:
        transform var(--sc-duration-normal) var(--sc-ease-out),
        opacity var(--sc-duration-normal);
    }

    .toast.exiting {
      animation: sc-slide-out-down var(--sc-duration-fast) var(--sc-ease-in) forwards;
    }

    @media (prefers-reduced-motion: reduce) {
      .toast {
        transition: none;
      }
      .toast.exiting {
        animation: sc-fade-out var(--sc-duration-instant) forwards;
      }
    }

    .toast.variant-info {
      border-left: 4px solid var(--sc-info);
    }

    .toast.variant-success {
      border-left: 4px solid var(--sc-success);
    }

    .toast.variant-warning {
      border-left: 4px solid var(--sc-warning);
    }

    .toast.variant-error {
      border-left: 4px solid var(--sc-error);
    }

    .message {
      flex: 1;
      color: var(--sc-text);
      line-height: var(--sc-leading-normal);
    }

    .actions {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      flex-shrink: 0;
    }

    .action-btn {
      padding: var(--sc-space-xs) var(--sc-space-sm);
      background: transparent;
      border: none;
      border-radius: var(--sc-radius-sm);
      color: var(--sc-accent-text, var(--sc-accent));
      font-family: var(--sc-font);
      font-size: var(--sc-text-xs);
      font-weight: var(--sc-weight-medium);
      cursor: pointer;
      transition: color var(--sc-duration-fast);
    }

    .action-btn:hover {
      color: var(--sc-accent-hover, var(--sc-accent));
    }

    .dismiss-btn {
      padding: var(--sc-space-xs);
      background: transparent;
      border: none;
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      transition: color var(--sc-duration-fast);
      line-height: 1;
    }

    .dismiss-btn:hover {
      color: var(--sc-text);
    }

    .dismiss-btn svg {
      width: 16px;
      height: 16px;
    }
  `;

  @state() private _toasts: ToastItem[] = [];
  private _counter = 0;
  private _timeouts = new Map<string, ReturnType<typeof setTimeout>>();

  /** Show a toast. Appends singleton container to body if needed. */
  static show(options: ToastOptions): void {
    let container = document.querySelector<ScToast>("sc-toast");
    if (!container) {
      container = document.createElement("sc-toast");
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
            class="toast variant-${item.variant} ${item.visible ? "" : "exiting"}"
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
    "sc-toast": ScToast;
  }
}
