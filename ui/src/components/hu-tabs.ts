import { LitElement, html, css } from "lit";
import { customElement, property, state } from "lit/decorators.js";

export interface TabItem {
  id: string;
  label: string;
}

@customElement("hu-tabs")
export class ScTabs extends LitElement {
  static override styles = css`
    :host {
      display: block;
      font-family: var(--hu-font);
    }

    .tablist {
      display: flex;
      position: relative;
      gap: var(--hu-space-md);
      border-bottom: 1px solid var(--hu-border);
    }

    .tab {
      position: relative;
      padding: var(--hu-space-md) 0;
      background: none;
      border: none;
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text-muted);
      cursor: pointer;
      transition: color var(--hu-duration-fast);
      outline: none;
    }

    .tab:hover {
      color: var(--hu-text);
    }

    .tab:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .tab[aria-selected="true"] {
      color: var(--hu-text);
    }

    @media (prefers-reduced-motion: reduce) {
      .indicator {
        transition: none;
      }
    }

    .indicator {
      position: absolute;
      bottom: -1px;
      height: 2px;
      background: var(--hu-accent);
      border-radius: var(--hu-radius-sm) var(--hu-radius-sm) 0 0;
      transition:
        left var(--hu-duration-normal) var(--hu-ease-out),
        width var(--hu-duration-normal) var(--hu-ease-out);
    }
  `;

  @property({ type: String }) value = "";
  @property({ type: Array }) tabs: TabItem[] = [];

  @state() private _indicatorLeft = 0;
  @state() private _indicatorWidth = 0;

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("value") || changedProperties.has("tabs")) {
      this.updateComplete.then(() => requestAnimationFrame(() => this._updateIndicator()));
    }
  }

  override firstUpdated(): void {
    requestAnimationFrame(() => this._updateIndicator());
    const ro = new ResizeObserver(() => this._updateIndicator());
    ro.observe(this);
  }

  private _updateIndicator(): void {
    if (!this.value) {
      this._indicatorLeft = 0;
      this._indicatorWidth = 0;
      return;
    }
    const el = this.renderRoot.querySelector<HTMLElement>(`[data-tab-id="${this.value}"]`);
    if (el) {
      const rect = el.getBoundingClientRect();
      const container = this.renderRoot.querySelector<HTMLElement>(".tablist");
      const containerRect = container?.getBoundingClientRect();
      if (containerRect) {
        this._indicatorLeft = rect.left - containerRect.left;
        this._indicatorWidth = rect.width;
      }
    }
  }

  private _onTabClick(id: string): void {
    this.value = id;
    this.dispatchEvent(
      new CustomEvent("tab-change", {
        detail: id,
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onKeyDown(e: KeyboardEvent): void {
    const tabIds = this.tabs.map((t) => t.id);
    const idx = tabIds.indexOf(this.value);
    let nextIdx = idx;
    if (e.key === "ArrowLeft" || e.key === "ArrowUp") {
      e.preventDefault();
      nextIdx = idx <= 0 ? tabIds.length - 1 : idx - 1;
    } else if (e.key === "ArrowRight" || e.key === "ArrowDown") {
      e.preventDefault();
      nextIdx = idx >= tabIds.length - 1 ? 0 : idx + 1;
    } else if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      this._onTabClick(tabIds[idx] ?? "");
      return;
    }
    if (nextIdx >= 0 && nextIdx < tabIds.length) {
      this.value = tabIds[nextIdx];
      this.dispatchEvent(
        new CustomEvent("tab-change", {
          detail: this.value,
          bubbles: true,
          composed: true,
        }),
      );
      this.updateComplete.then(() => this._updateIndicator());
    }
  }

  override render() {
    return html`
      <div class="tablist" role="tablist" @keydown=${this._onKeyDown}>
        ${this.tabs.map(
          (tab) => html`
            <button
              class="tab"
              role="tab"
              data-tab-id=${tab.id}
              aria-selected=${this.value === tab.id}
              id="tab-${tab.id}"
              tabindex=${this.value === tab.id ? 0 : -1}
              @click=${() => this._onTabClick(tab.id)}
            >
              ${tab.label}
            </button>
          `,
        )}
        ${this.value
          ? html`
              <div
                class="indicator"
                role="presentation"
                style="left: ${this._indicatorLeft}px; width: ${this._indicatorWidth}px;"
              ></div>
            `
          : null}
      </div>
      <slot name="panel"></slot>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-tabs": ScTabs;
  }
}
