import { LitElement, html, css, nothing } from "lit";
import type { TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";

type SectionId =
  | "config"
  | "models"
  | "tools"
  | "channels"
  | "skills"
  | "automations"
  | "security"
  | "nodes"
  | "usage"
  | "observability"
  | "logs"
  | "sessions"
  | "overview"
  | "turing"
  | "hula"
  | "connectors"
  | "design-system";

type SettingsGroup = "system" | "capabilities" | "observability" | "developer";

interface SettingsSection {
  id: SectionId;
  label: string;
  icon: ReturnType<typeof html>;
  group: SettingsGroup;
  load: () => Promise<unknown>;
  render: () => TemplateResult;
}

const SETTINGS_GROUP_ORDER: { key: SettingsGroup; label: string }[] = [
  { key: "system", label: "System" },
  { key: "capabilities", label: "Capabilities" },
  { key: "observability", label: "Observability" },
  { key: "developer", label: "Developer" },
];

const SETTINGS_SECTIONS: SettingsSection[] = [
  /* SYSTEM */
  {
    id: "config",
    label: "Config",
    icon: icons.settings,
    group: "system",
    load: () => import("./config-view.js"),
    render: () => html`<hu-config-view></hu-config-view>`,
  },
  {
    id: "models",
    label: "Models",
    icon: icons.cpu,
    group: "system",
    load: () => import("./models-view.js"),
    render: () => html`<hu-models-view></hu-models-view>`,
  },
  {
    id: "security",
    label: "Security",
    icon: icons.shield,
    group: "system",
    load: () => import("./security-view.js"),
    render: () => html`<hu-security-view></hu-security-view>`,
  },
  {
    id: "nodes",
    label: "Nodes",
    icon: icons.server,
    group: "system",
    load: () => import("./nodes-view.js"),
    render: () => html`<hu-nodes-view></hu-nodes-view>`,
  },
  /* CAPABILITIES */
  {
    id: "tools",
    label: "Tools",
    icon: icons.wrench,
    group: "capabilities",
    load: () => import("./tools-view.js"),
    render: () => html`<hu-tools-view></hu-tools-view>`,
  },
  {
    id: "channels",
    label: "Channels",
    icon: icons.radio,
    group: "capabilities",
    load: () => import("./channels-view.js"),
    render: () => html`<hu-channels-view></hu-channels-view>`,
  },
  {
    id: "skills",
    label: "Skills",
    icon: icons.puzzle,
    group: "capabilities",
    load: () => import("./skills-view.js"),
    render: () => html`<hu-skills-view></hu-skills-view>`,
  },
  {
    id: "automations",
    label: "Automations",
    icon: icons.timer,
    group: "capabilities",
    load: () => import("./automations-view.js"),
    render: () => html`<hu-automations-view></hu-automations-view>`,
  },
  {
    id: "hula",
    label: "HuLa",
    icon: icons.code,
    group: "capabilities",
    load: () => import("./hula-view.js"),
    render: () => html`<hu-hula-view></hu-hula-view>`,
  },
  {
    id: "connectors",
    label: "Connectors",
    icon: icons.compass,
    group: "capabilities",
    load: () => import("./connectors-view.js"),
    render: () => html`<hu-connectors-view></hu-connectors-view>`,
  },
  /* OBSERVABILITY */
  {
    id: "overview",
    label: "Overview",
    icon: icons.grid,
    group: "observability",
    load: () => import("./overview-view.js"),
    render: () => html`<hu-overview-view></hu-overview-view>`,
  },
  {
    id: "usage",
    label: "Usage",
    icon: icons["bar-chart"],
    group: "observability",
    load: () => import("./usage-view.js"),
    render: () => html`<hu-usage-view></hu-usage-view>`,
  },
  {
    id: "turing",
    label: "Turing",
    icon: icons["chart-line"],
    group: "observability",
    load: () => import("./turing-view.js"),
    render: () => html`<hu-turing-view></hu-turing-view>`,
  },
  {
    id: "observability",
    label: "Observability",
    icon: icons["chart-line"],
    group: "observability",
    load: () => import("./metrics-view.js"),
    render: () => html`<hu-metrics-view></hu-metrics-view>`,
  },
  /* DEVELOPER */
  {
    id: "sessions",
    label: "Sessions",
    icon: icons["clock-counter-clockwise"],
    group: "developer",
    load: () => import("./sessions-view.js"),
    render: () => html`<hu-sessions-view></hu-sessions-view>`,
  },
  {
    id: "logs",
    label: "Logs",
    icon: icons.terminal,
    group: "developer",
    load: () => import("./logs-view.js"),
    render: () => html`<hu-logs-view></hu-logs-view>`,
  },
  {
    id: "design-system",
    label: "Design System",
    icon: icons.grid,
    group: "developer",
    load: () => import("./design-system-view.js"),
    render: () => html`<hu-design-system-view></hu-design-system-view>`,
  },
];

@customElement("hu-settings-view")
export class ScSettingsView extends LitElement {
  static override styles = css`
    :host {
      view-transition-name: view-settings;
      display: block;
      container-type: inline-size;
    }

    .header-area {
      padding: var(--hu-space-md) var(--hu-space-lg) 0;
      max-width: var(--hu-content-width-wide);
      margin: 0 auto;
    }

    .tabs {
      display: flex;
      gap: var(--hu-space-xs);
      flex-wrap: nowrap;
      align-items: center;
      padding: 0 var(--hu-space-lg);
      max-width: var(--hu-content-width-wide);
      overflow-x: auto;
      scrollbar-width: none;
      -ms-overflow-style: none;
      margin: 0 auto var(--hu-space-lg);
      border-bottom: 1px solid var(--hu-border-subtle);
      padding-bottom: var(--hu-space-sm);
    }

    .tabs::-webkit-scrollbar {
      display: none;
    }

    .tab-group-label {
      font-size: var(--hu-text-xs);
      font-weight: var(--hu-weight-semibold);
      font-family: var(--hu-font);
      color: var(--hu-text-faint);
      text-transform: uppercase;
      letter-spacing: 0.04em;
      padding: var(--hu-space-2xs) var(--hu-space-xs);
      white-space: nowrap;
      user-select: none;
    }

    .tab-group-divider {
      width: 1px;
      height: 1.25rem;
      background: var(--hu-border-subtle);
      flex-shrink: 0;
      margin: 0 var(--hu-space-xs);
    }

    /* Mobile accordion */
    .tabs-accordion {
      display: none;
    }

    @media (max-width: 599px) /* --hu-breakpoint-compact */ {
      .tabs {
        display: none;
      }

      .tabs-accordion {
        display: block;
        padding: 0 var(--hu-space-lg);
        max-width: var(--hu-content-width-wide);
        margin: 0 auto var(--hu-space-md);
      }

      .accordion-group {
        border-bottom: 1px solid var(--hu-border-subtle);
      }

      .accordion-group:last-child {
        border-bottom: none;
      }

      .accordion-trigger {
        display: flex;
        align-items: center;
        justify-content: space-between;
        width: 100%;
        padding: var(--hu-space-sm) 0;
        background: transparent;
        border: none;
        font-size: var(--hu-text-sm);
        font-weight: var(--hu-weight-semibold);
        font-family: var(--hu-font);
        color: var(--hu-text-secondary);
        text-transform: uppercase;
        letter-spacing: 0.04em;
        cursor: pointer;
        transition: color var(--hu-duration-fast) var(--hu-ease-out);
      }

      .accordion-trigger:hover {
        color: var(--hu-text);
      }

      .accordion-trigger:focus-visible {
        outline: var(--hu-focus-ring-width) solid var(--hu-accent);
        outline-offset: calc(-1 * var(--hu-focus-ring-width));
      }

      .accordion-chevron {
        width: var(--hu-icon-xs);
        height: var(--hu-icon-xs);
        transition: transform var(--hu-duration-fast) var(--hu-ease-out);
      }

      .accordion-trigger[aria-expanded="true"] .accordion-chevron {
        transform: rotate(180deg);
      }

      .accordion-items {
        display: none;
        flex-direction: column;
        gap: var(--hu-space-2xs);
        padding-bottom: var(--hu-space-sm);
      }

      .accordion-items[data-open] {
        display: flex;
      }
    }

    .tab-btn {
      display: flex;
      align-items: center;
      gap: 0.25rem;
      padding: 0.375rem 0.625rem;
      background: transparent;
      flex-shrink: 0;
      white-space: nowrap;
      border: none;
      border-radius: var(--hu-radius);
      font-size: var(--hu-text-sm);
      font-family: var(--hu-font);
      color: var(--hu-text-secondary);
      cursor: pointer;
      white-space: nowrap;
      transition:
        background var(--hu-duration-fast) var(--hu-ease-out),
        color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .tab-btn:hover {
      background: var(--hu-hover-overlay);
      color: var(--hu-text);
    }

    .tab-btn:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .tab-btn[aria-selected="true"] {
      color: var(--hu-accent-text, var(--hu-accent));
      background: color-mix(in srgb, var(--hu-accent) 8%, transparent);
      font-weight: var(--hu-weight-medium, 500);
    }

    .tab-btn .icon {
      width: 0.875rem;
      height: 0.875rem;
    }

    .tab-btn .icon svg {
      width: 100%;
      height: 100%;
    }

    .content {
      min-height: 20rem;
      animation: hu-settings-fade var(--hu-duration-fast, 150ms) var(--hu-ease-out) both;
    }
    @keyframes hu-settings-fade {
      from {
        opacity: 0;
      }
      to {
        opacity: 1;
      }
    }

    .loading {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
      padding: var(--hu-space-lg) 0;
      min-height: 20rem;
    }

    @media (prefers-reduced-motion: reduce) {
      .content {
        animation: none;
      }
    }
  `;

  @state() private _activeSection: SectionId = "config";
  @state() private _loading = false;
  @state() private _openGroups = new Set<SettingsGroup>(["system"]);
  private _loadedSections = new Set<SectionId>();

  override render() {
    const active =
      SETTINGS_SECTIONS.find((s) => s.id === this._activeSection) ?? SETTINGS_SECTIONS[0];
    return html`
      <div class="header-area">
        <hu-page-hero>
          <hu-section-header
            heading="Settings"
            description="Configure your h-uman instance"
          ></hu-section-header>
        </hu-page-hero>
      </div>

      <!-- Desktop: grouped horizontal tabs -->
      <nav class="tabs" role="tablist" aria-label="Settings sections">
        ${SETTINGS_GROUP_ORDER.map((group, gi) => {
          const sections = SETTINGS_SECTIONS.filter((s) => s.group === group.key);
          return html`
            ${gi > 0 ? html`<span class="tab-group-divider" aria-hidden="true"></span>` : nothing}
            <span class="tab-group-label" aria-hidden="true">${group.label}</span>
            ${sections.map(
              (s) => html`
                <button
                  class="tab-btn"
                  role="tab"
                  aria-selected=${String(s.id === this._activeSection)}
                  @click=${() => void this._selectSection(s.id)}
                >
                  <span class="icon">${s.icon}</span>
                  ${s.label}
                </button>
              `,
            )}
          `;
        })}
      </nav>

      <!-- Mobile: accordion groups -->
      <nav class="tabs-accordion" aria-label="Settings sections">
        ${SETTINGS_GROUP_ORDER.map((group) => {
          const sections = SETTINGS_SECTIONS.filter((s) => s.group === group.key);
          const isOpen = this._openGroups.has(group.key);
          return html`
            <div class="accordion-group">
              <button
                class="accordion-trigger"
                aria-expanded=${String(isOpen)}
                @click=${() => this._toggleGroup(group.key)}
              >
                ${group.label}
                <svg
                  class="accordion-chevron"
                  viewBox="0 0 256 256"
                  fill="currentColor"
                  aria-hidden="true"
                >
                  <path
                    d="M213.66,101.66l-80,80a8,8,0,0,1-11.32,0l-80-80A8,8,0,0,1,53.66,90.34L128,164.69l74.34-74.35a8,8,0,0,1,11.32,11.32Z"
                  ></path>
                </svg>
              </button>
              <div class="accordion-items" ?data-open=${isOpen}>
                ${sections.map(
                  (s) => html`
                    <button
                      class="tab-btn"
                      role="tab"
                      aria-selected=${String(s.id === this._activeSection)}
                      @click=${() => void this._selectSection(s.id)}
                    >
                      <span class="icon">${s.icon}</span>
                      ${s.label}
                    </button>
                  `,
                )}
              </div>
            </div>
          `;
        })}
      </nav>

      <div class="content" role="tabpanel" aria-label=${active.label}>
        ${
          this._loading
            ? html`<div class="loading">
                <hu-skeleton variant="line" width="40%" height="1rem"></hu-skeleton>
                <hu-skeleton variant="line" width="100%" height="2.5rem"></hu-skeleton>
                <hu-skeleton variant="line" width="100%" height="2.5rem"></hu-skeleton>
                <hu-skeleton variant="line" width="60%" height="2.5rem"></hu-skeleton>
              </div>`
            : this._renderSection(active)
        }
      </div>
    `;
  }

  private _toggleGroup(group: SettingsGroup): void {
    const next = new Set(this._openGroups);
    if (next.has(group)) {
      next.delete(group);
    } else {
      next.add(group);
    }
    this._openGroups = next;
  }

  private _renderSection(section: SettingsSection) {
    if (!this._loadedSections.has(section.id)) return nothing;
    return section.render();
  }

  private async _selectSection(id: SectionId): Promise<void> {
    if (id === this._activeSection && this._loadedSections.has(id)) return;
    this._activeSection = id;
    const section = SETTINGS_SECTIONS.find((s) => s.id === id);
    if (!section) return;
    if (!this._loadedSections.has(id)) {
      this._loading = true;
      try {
        await section.load();
        this._loadedSections.add(id);
      } catch {
        /* dynamic import failed — section stays unloaded, user can retry */
      } finally {
        this._loading = false;
      }
    }
  }

  override firstUpdated(): void {
    queueMicrotask(() => void this._selectSection(this._activeSection));
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-settings-view": ScSettingsView;
  }
}
