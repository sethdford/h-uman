import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";

export type BreadcrumbItem = {
  label: string;
  href?: string;
};

@customElement("hu-breadcrumb")
export class ScBreadcrumb extends LitElement {
  @property({ type: Array }) items: BreadcrumbItem[] = [];

  static override styles = css`
    :host {
      display: block;
    }

    nav {
      font-size: var(--hu-text-sm);
      font-family: var(--hu-font);
    }

    .list {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: var(--hu-space-xs);
      list-style: none;
      margin: 0;
      padding: 0;
    }

    .item {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
    }

    .item:not(:last-child)::after {
      content: "/";
      color: var(--hu-text-faint);
      margin-left: var(--hu-space-xs);
    }

    .link {
      color: var(--hu-text-muted);
      text-decoration: none;
      transition: color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .link:hover {
      color: var(--hu-accent-text, var(--hu-accent));
    }

    .link:focus-visible {
      outline: var(--hu-focus-ring-width) solid var(--hu-focus-ring);
      outline-offset: var(--hu-focus-ring-offset);
    }

    .current {
      color: var(--hu-text);
    }

    @media (prefers-reduced-motion: reduce) {
      .link {
        transition: none;
      }
    }
  `;

  private _onClick(e: Event, item: BreadcrumbItem): void {
    if (item.href) {
      e.preventDefault();
      this.dispatchEvent(
        new CustomEvent("hu-navigate", {
          bubbles: true,
          composed: true,
          detail: { href: item.href },
        }),
      );
    }
  }

  override render() {
    return html`
      <nav aria-label="Breadcrumb">
        <ol class="list">
          ${this.items.map((item, i) => {
            const isLast = i === this.items.length - 1;
            return html`
              <li class="item">
                ${isLast
                  ? html`<span class="current" aria-current="page">${item.label}</span>`
                  : item.href
                    ? html`
                        <a
                          class="link"
                          href=${item.href}
                          @click=${(e: Event) => this._onClick(e, item)}
                          >${item.label}</a
                        >
                      `
                    : html`<span class="current">${item.label}</span>`}
              </li>
            `;
          })}
        </ol>
      </nav>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-breadcrumb": ScBreadcrumb;
  }
}
