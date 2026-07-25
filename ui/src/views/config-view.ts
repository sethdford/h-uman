import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import type { PropertyValues } from "lit";
import { scrollEntranceStyles } from "../styles/scroll-entrance.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { friendlyError } from "../utils/friendly-error.js";
import { log } from "../lib/log.js";
import { ScToast } from "../components/hu-toast.js";
import { icons } from "../icons.js";
import "../components/hu-page-hero.js";
import "../components/hu-section-header.js";
import "../components/hu-card.js";
import "../components/hu-badge.js";
import "../components/hu-input.js";
import "../components/hu-button.js";
import "../components/hu-skeleton.js";
import "../components/hu-form-group.js";
import "../components/hu-combobox.js";
import "../components/hu-code-block.js";
import "../components/hu-empty-state.js";

type SaveStatus = "saved" | "error" | "unsaved" | "idle";

interface SchemaProperty {
  type?: string;
  description?: string;
}

interface ConfigSchema {
  type?: string;
  properties?: Record<string, SchemaProperty>;
}

interface ConfigData {
  exists?: boolean;
  workspace_dir?: string;
  default_provider?: string;
  default_model?: string;
  max_tokens?: number;
  temperature?: number;
}

interface ProviderItem {
  name?: string;
}

const DEFAULT_PROVIDERS: { value: string; label: string }[] = [
  { value: "openai", label: "OpenAI" },
  { value: "anthropic", label: "Anthropic" },
  { value: "gemini", label: "Gemini" },
  { value: "ollama", label: "Ollama" },
  { value: "openrouter", label: "OpenRouter" },
  { value: "compatible", label: "Compatible" },
];

const COMMON_MODELS: { value: string; label: string }[] = [
  { value: "gpt-4o", label: "GPT-4o" },
  { value: "gpt-4o-mini", label: "GPT-4o Mini" },
  { value: "claude-sonnet-4-20250514", label: "Claude Sonnet 4" },
  { value: "claude-3-5-sonnet-20241022", label: "Claude 3.5 Sonnet" },
  { value: "gemini-3.1-pro-preview", label: "Gemini 3.1 Pro" },
  { value: "gemini-3-flash-preview", label: "Gemini 3 Flash" },
  { value: "gemini-3.1-flash-lite-preview", label: "Gemini 3.1 Flash Lite" },
  { value: "llama-3.1-70b", label: "Llama 3.1 70B" },
];

function toRawConfig(edited: ConfigData): Record<string, unknown> {
  return {
    workspace: edited.workspace_dir ?? ".",
    default_provider: edited.default_provider ?? "openai",
    default_model: edited.default_model ?? "",
    default_temperature: edited.temperature ?? 0.7,
    max_tokens: edited.max_tokens ?? 0,
  };
}

@customElement("hu-config-view")
export class ScConfigView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  private _scrollEntranceObserver: IntersectionObserver | null = null;

  static override styles = [
    scrollEntranceStyles,
    css`
      :host {
        view-transition-name: view-config;
        display: block;
        min-width: 0;
        width: 100%;
        max-width: 40rem;
        contain: layout style;
        container-type: inline-size;
        margin: 0 auto;
      }
      .header-actions {
        display: flex;
        gap: var(--hu-space-sm);
        align-items: center;
      }
      .form {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-adaptive-section-gap);
      }
      .section {
        border: 1px solid var(--hu-border);
        border-radius: var(--hu-radius);
        overflow: hidden;
        background: var(--hu-surface-container);
      }
      .section + .section {
        margin-top: var(--hu-space-sm);
      }
      .section-header {
        padding: var(--hu-space-md) var(--hu-space-md);
        background: var(--hu-surface-container);
        cursor: pointer;
        display: flex;
        align-items: center;
        justify-content: space-between;
        user-select: none;
        transition: background var(--hu-duration-fast) var(--hu-ease-out);

        &:hover {
          background: var(--hu-bg-overlay);
        }

        &:focus-visible {
          outline: 2px solid var(--hu-accent);
          outline-offset: -2px;
        }

        & .chevron {
          transition: transform var(--hu-duration-normal) var(--hu-ease-out);
          color: var(--hu-text-muted);
          display: flex;

          & svg {
            width: 0.875rem;
            height: 0.875rem;
          }
        }

        &.collapsed .chevron {
          transform: rotate(-90deg);
        }
      }
      .section-content {
        padding: var(--hu-space-md) var(--hu-space-lg);
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-md);
        max-height: 37.5rem;
        overflow: hidden;
        transition:
          max-height var(--hu-duration-slow) var(--hu-ease-out),
          padding var(--hu-duration-normal) var(--hu-ease-out),
          opacity var(--hu-duration-normal) var(--hu-ease-out);
      }
      .section.collapsed .section-content {
        max-height: 0;
        padding-top: 0;
        padding-bottom: 0;
        opacity: 0;
      }
      .field {
        display: flex;
        flex-direction: column;
        gap: var(--hu-space-xs);

        & label {
          font-size: var(--hu-text-base);
          font-weight: var(--hu-weight-medium);
          color: var(--hu-text);
        }

        & .description {
          font-size: var(--hu-text-xs);
          color: var(--hu-text-muted);
          margin-top: var(--hu-space-2xs);
        }

        & .description-error {
          color: var(--hu-error);
        }
      }
      .raw-area {
        --hu-font: var(--hu-font-mono);
        --hu-text-base: var(--hu-text-sm);
        --hu-bg-elevated: var(--hu-bg-inset);
        width: 100%;
      }
      .unsaved-banner {
        position: sticky;
        bottom: 0;
        left: 0;
        right: 0;
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: var(--hu-space-md) var(--hu-space-lg);
        background: var(--hu-bg-surface);
        border-top: 1px solid var(--hu-border);
        box-shadow: 0 -4px 16px color-mix(in srgb, var(--hu-shadow-color, #000) 12%, transparent);
        z-index: 10;
      }
      .unsaved-banner-actions {
        display: flex;
        gap: var(--hu-space-sm);
      }
      @container (max-width: 30rem) /* cq-sm */ {
        :host {
          max-width: 100%;
        }
      }
      @media (prefers-reduced-motion: reduce) {
        *,
        *::before,
        *::after {
          animation-duration: 0s !important;
          transition-duration: 0s !important;
        }
      }
    `,
  ];

  override updated(changedProperties: PropertyValues): void {
    super.updated(changedProperties);
    this.updateComplete.then(() => this._setupScrollEntrance());
  }

  private _setupScrollEntrance(): void {
    if (typeof CSS !== "undefined" && CSS.supports?.("animation-timeline", "view()")) return;
    const root = this.renderRoot;
    if (!root) return;
    const elements = root.querySelectorAll(".hu-scroll-reveal-stagger > *");
    if (elements.length === 0) return;
    if (!this._scrollEntranceObserver) {
      this._scrollEntranceObserver = new IntersectionObserver(
        (entries) => {
          entries.forEach((e) => {
            if (e.isIntersecting) {
              (e.target as HTMLElement).classList.add("entered");
              this._scrollEntranceObserver?.unobserve(e.target);
            }
          });
        },
        { threshold: 0.1 },
      );
    }
    elements.forEach((el) => this._scrollEntranceObserver!.observe(el));
  }

  @state() private loading = true;
  @state() private config: ConfigData = {};
  @state() private edited: ConfigData = {};
  @state() private schema: ConfigSchema = {};
  @state() private rawMode = false;
  @state() private rawText = "";
  @state() private rawEditing = false;
  @state() private sectionCollapsed = false;
  @state() private saveStatus: SaveStatus = "idle";
  @state() private errorMessage = "";
  @state() private loadError = "";
  @state() private saving = false;
  @state() private providerOptions: { value: string; label: string }[] = DEFAULT_PROVIDERS;
  @state() private modelOptions: { value: string; label: string }[] = COMMON_MODELS;
  @state() private temperatureError = "";
  private _beforeUnloadHandler?: (e: BeforeUnloadEvent) => void;

  override connectedCallback(): void {
    super.connectedCallback();
    this._beforeUnloadHandler = (e: BeforeUnloadEvent) => {
      if (this.hasChanges()) {
        e.preventDefault();
      }
    };
    window.addEventListener("beforeunload", this._beforeUnloadHandler);
  }

  override disconnectedCallback(): void {
    this._scrollEntranceObserver?.disconnect();
    this._scrollEntranceObserver = null;
    if (this._beforeUnloadHandler) {
      window.removeEventListener("beforeunload", this._beforeUnloadHandler);
    }
    super.disconnectedCallback();
  }

  protected override async load(): Promise<void> {
    this.loading = true;
    this.loadError = "";
    await Promise.all([this.loadConfig(), this.loadSchema(), this.loadProviderOptions()]);
    this.loading = false;
  }

  private async loadConfig(): Promise<void> {
    if (!this.gateway) {
      this.loadError = "Not connected to gateway";
      return;
    }
    try {
      const cfg = await this.gateway.request<Partial<ConfigData>>("config.get", {});
      this.config = {
        exists: cfg?.exists ?? false,
        workspace_dir: cfg?.workspace_dir ?? "",
        default_provider: cfg?.default_provider ?? "",
        default_model: cfg?.default_model ?? "",
        max_tokens: cfg?.max_tokens ?? 0,
        temperature: cfg?.temperature ?? 0.7,
      };
      this.edited = { ...this.config };
      this.rawText = JSON.stringify(toRawConfig(this.config), null, 2);
      this.saveStatus = "idle";
    } catch (e) {
      this.saveStatus = "error";
      this.errorMessage = friendlyError(e);
      this.loadError = friendlyError(e);
    }
  }

  private async loadSchema(): Promise<void> {
    if (!this.gateway) return;
    try {
      const res = await this.gateway.request<{
        schema?: ConfigSchema;
      }>("config.schema", {});
      const s = res?.schema;
      this.schema = s ?? { type: "object", properties: {} };
    } catch (e) {
      log.warn("[config-view] failed to load schema, using defaults:", e);
      this.schema = {
        type: "object",
        properties: {
          workspace_dir: { type: "string", description: "Workspace directory" },
          default_provider: {
            type: "string",
            description: "Default AI provider",
          },
          default_model: { type: "string", description: "Default model" },
          max_tokens: {
            type: "integer",
            description: "Max tokens per response",
          },
          temperature: {
            type: "number",
            description: "Temperature (0.0 - 2.0)",
          },
        },
      };
    }
  }

  private async loadProviderOptions(): Promise<void> {
    if (!this.gateway) return;
    try {
      const res = await this.gateway.request<{ providers?: ProviderItem[] }>("models.list", {});
      const providers = res?.providers ?? [];
      if (providers.length > 0) {
        this.providerOptions = providers
          .map((p) => ({
            value: p.name ?? "",
            label: (p.name ?? "").charAt(0).toUpperCase() + (p.name ?? "").slice(1),
          }))
          .filter((o) => o.value);
      }
    } catch (e) {
      log.warn("[config-view] failed to load provider options, using defaults:", e);
    }
  }

  private hasChanges(): boolean {
    if (this.rawMode) {
      try {
        const parsed = JSON.parse(this.rawText) as Record<string, unknown>;
        const current = toRawConfig(this.config);
        return JSON.stringify(parsed) !== JSON.stringify(current);
      } catch (e) {
        log.warn("[config-view] malformed JSON in raw mode:", e);
        return this.rawText.trim().length > 0;
      }
    }
    return (
      (this.edited.workspace_dir ?? "") !== (this.config.workspace_dir ?? "") ||
      (this.edited.default_provider ?? "") !== (this.config.default_provider ?? "") ||
      (this.edited.default_model ?? "") !== (this.config.default_model ?? "") ||
      (this.edited.max_tokens ?? 0) !== (this.config.max_tokens ?? 0) ||
      Math.abs((this.edited.temperature ?? 0.7) - (this.config.temperature ?? 0.7)) > 0.001
    );
  }

  private validateTemperature(): void {
    const t = this.edited.temperature ?? 0.7;
    if (t < 0 || t > 2) {
      this.temperatureError = "Temperature must be between 0 and 2";
    } else {
      this.temperatureError = "";
    }
  }

  private async save(): Promise<void> {
    if (!this.gateway) return;
    if (!this.rawMode) {
      this.validateTemperature();
      if (this.temperatureError) {
        ScToast.show({ message: this.temperatureError, variant: "error" });
        return;
      }
    }
    this.saving = true;
    this.saveStatus = "idle";
    this.errorMessage = "";
    try {
      let raw: string;
      if (this.rawMode) {
        raw = this.rawText;
        try {
          JSON.parse(raw);
        } catch {
          this.saveStatus = "error";
          this.errorMessage = "Invalid JSON";
          ScToast.show({ message: "Invalid JSON", variant: "error" });
          return;
        }
      } else {
        raw = JSON.stringify(toRawConfig(this.edited));
      }
      const payload = await this.gateway.request<{ saved?: boolean }>("config.set", { raw });
      if (payload?.saved !== false) {
        if (this.rawMode) {
          try {
            const parsed = JSON.parse(raw) as Record<string, unknown>;
            this.config = {
              exists: true,
              workspace_dir: String(parsed.workspace ?? "."),
              default_provider: String(parsed.default_provider ?? "openai"),
              default_model: String(parsed.default_model ?? ""),
              max_tokens: Number(parsed.max_tokens ?? 0),
              temperature: Number(parsed.default_temperature ?? parsed.temperature ?? 0.7),
            };
            this.edited = { ...this.config };
          } catch {
            /* keep config as-is */
          }
        } else {
          this.config = { ...this.edited };
        }
        this.rawText = JSON.stringify(toRawConfig(this.config), null, 2);
        this.rawEditing = false;
        this.saveStatus = "saved";
        ScToast.show({ message: "Config saved", variant: "success" });
        setTimeout(() => {
          if (this.saveStatus === "saved") this.saveStatus = "idle";
        }, 2000);
      } else {
        this.saveStatus = "error";
        this.errorMessage = "Save rejected";
        ScToast.show({ message: "Save rejected by server", variant: "error" });
      }
    } catch (e) {
      this.saveStatus = "error";
      this.errorMessage = friendlyError(e);
      ScToast.show({ message: this.errorMessage, variant: "error" });
    } finally {
      this.saving = false;
    }
  }

  private revert(): void {
    this.edited = { ...this.config };
    this.rawText = JSON.stringify(toRawConfig(this.config), null, 2);
    this.rawEditing = false;
    this.temperatureError = "";
    this.saveStatus = "idle";
    ScToast.show({ message: "Changes reverted", variant: "success" });
  }

  private toggleRawMode(): void {
    if (this.rawMode) {
      try {
        const parsed = JSON.parse(this.rawText) as Record<string, unknown>;
        this.edited = {
          exists: true,
          workspace_dir: String(parsed.workspace ?? "."),
          default_provider: String(parsed.default_provider ?? "openai"),
          default_model: String(parsed.default_model ?? ""),
          max_tokens: Number(parsed.max_tokens ?? 0),
          temperature: Number(parsed.default_temperature ?? parsed.temperature ?? 0.7),
        };
      } catch (e) {
        log.warn("[config-view] failed to parse raw JSON, keeping previous state:", e);
      }
    } else {
      this.rawText = JSON.stringify(toRawConfig(this.edited), null, 2);
    }
    this.rawMode = !this.rawMode;
    this.rawEditing = false;
  }

  override render() {
    if (this.loading) return this._renderSkeleton();
    if (this.loadError)
      return html`<hu-empty-state
        .icon=${icons.warning}
        heading="Error"
        description=${this.loadError}
      ></hu-empty-state>`;
    if (this.rawMode) return this._renderRaw();
    return this._renderForm();
  }

  private _renderSkeleton(): TemplateResult {
    return html`
      <hu-page-hero role="region" aria-label="Configuration">
        <hu-section-header
          heading="Configuration"
          description="Manage your h-uman instance settings"
        ></hu-section-header>
      </hu-page-hero>
      <div class="form hu-scroll-reveal-stagger">
        <hu-skeleton variant="card" height="200px"></hu-skeleton>
        <hu-skeleton variant="card" height="200px"></hu-skeleton>
      </div>
    `;
  }

  private _renderSaveStatusBadge(): TemplateResult | typeof nothing {
    if (this.saving) return html`<hu-badge variant="warning">Saving...</hu-badge>`;
    if (this.saveStatus === "saved") return html`<hu-badge variant="success">Saved</hu-badge>`;
    if (this.hasChanges()) return html`<hu-badge variant="warning">Unsaved changes</hu-badge>`;
    if (this.saveStatus === "error")
      return html`<hu-badge variant="error">${this.errorMessage || "Error"}</hu-badge>`;
    return nothing;
  }

  private _renderRaw(): TemplateResult {
    return html`
      <hu-page-hero role="region" aria-label="Configuration">
        <hu-section-header
          heading="Configuration"
          description="Manage your h-uman instance settings"
        >
          ${this._renderSaveStatusBadge()}
          <div class="header-actions">
            <hu-button
              variant="ghost"
              size="sm"
              @click=${this.toggleRawMode}
              aria-label=${this.rawMode ? "Switch to form editor" : "Switch to raw JSON editor"}
            >
              ${this.rawMode ? "Form" : "Raw JSON"}
            </hu-button>
            <hu-button
              variant="primary"
              ?disabled=${!this.hasChanges()}
              @click=${() => this.save()}
              aria-label="Save configuration"
            >
              Save
            </hu-button>
          </div>
        </hu-section-header>
      </hu-page-hero>
      <hu-card glass>
        <div class="form">
          ${
            this.rawEditing
              ? html`
                  <hu-textarea
                    class="raw-area"
                    .value=${this.rawText}
                    @hu-input=${(e: CustomEvent<{ value: string }>) => {
                      this.rawText = e.detail.value;
                      if (this.saveStatus === "saved") this.saveStatus = "idle";
                    }}
                    rows="12"
                    resize="vertical"
                  ></hu-textarea>
                  <hu-button
                    variant="secondary"
                    size="sm"
                    @click=${() => (this.rawEditing = false)}
                  >
                    Done editing
                  </hu-button>
                `
              : html`
                  <hu-code-block .code=${this.rawText} language="json"></hu-code-block>
                  <hu-button variant="secondary" size="sm" @click=${() => (this.rawEditing = true)}>
                    Edit
                  </hu-button>
                `
          }
        </div>
      </hu-card>
      ${
        this.hasChanges()
          ? html`
              <div class="unsaved-banner">
                <span>You have unsaved changes</span>
                <div class="unsaved-banner-actions">
                  <hu-button variant="secondary" @click=${this.revert}>Revert</hu-button>
                  <hu-button variant="primary" ?disabled=${this.saving} @click=${() => this.save()}>
                    Save
                  </hu-button>
                </div>
              </div>
            `
          : nothing
      }
    `;
  }

  private _onFormChange(): void {
    if (this.saveStatus === "saved") this.saveStatus = "idle";
    this.validateTemperature();
  }

  private _renderForm(): TemplateResult {
    return html`
      <hu-page-hero role="region" aria-label="Configuration">
        <hu-section-header
          heading="Configuration"
          description="Manage your h-uman instance settings"
        >
          ${this._renderSaveStatusBadge()}
          <div class="header-actions">
            <hu-button
              variant="ghost"
              size="sm"
              @click=${this.toggleRawMode}
              aria-label=${this.rawMode ? "Switch to form editor" : "Switch to raw JSON editor"}
            >
              ${this.rawMode ? "Form" : "Raw JSON"}
            </hu-button>
            <hu-button
              variant="primary"
              ?disabled=${!this.hasChanges()}
              @click=${() => this.save()}
              aria-label="Save configuration"
            >
              Save
            </hu-button>
          </div>
        </hu-section-header>
      </hu-page-hero>
      <div class="form hu-scroll-reveal-stagger">
        <hu-card glass>
          <hu-form-group title="Provider Settings" description="Default AI provider and model">
            <hu-combobox
              label="Provider"
              .options=${this.providerOptions}
              .value=${this.edited.default_provider ?? ""}
              placeholder="Select provider"
              freeText
              @hu-combobox-change=${(e: CustomEvent<{ value: string }>) => {
                this.edited = { ...this.edited, default_provider: e.detail.value };
                this._onFormChange();
              }}
            ></hu-combobox>
            <hu-combobox
              label="Model"
              .options=${this.modelOptions}
              .value=${this.edited.default_model ?? ""}
              placeholder="Select or type model"
              freeText
              @hu-combobox-change=${(e: CustomEvent<{ value: string }>) => {
                this.edited = { ...this.edited, default_model: e.detail.value };
                this._onFormChange();
              }}
            ></hu-combobox>
          </hu-form-group>
        </hu-card>

        <hu-card glass>
          <hu-form-group title="Model Settings" description="Token and temperature limits">
            <div class="field">
              <label for="max_tokens">Max tokens</label>
              <hu-input
                id="max_tokens"
                type="number"
                .min=${0}
                .value=${String(this.edited.max_tokens ?? 0)}
                @hu-input=${(e: CustomEvent<{ value: string }>) => {
                  const v = parseInt(e.detail.value, 10);
                  this.edited = { ...this.edited, max_tokens: isNaN(v) ? 0 : v };
                  this._onFormChange();
                }}
              ></hu-input>
            </div>
            <div class="field">
              <label for="temperature">Temperature (0–2)</label>
              <hu-input
                id="temperature"
                type="number"
                .min=${0}
                .max=${2}
                .step=${0.1}
                .value=${String(this.edited.temperature ?? 0.7)}
                .error=${this.temperatureError}
                @hu-input=${(e: CustomEvent<{ value: string }>) => {
                  const v = parseFloat(e.detail.value);
                  const clamped = isNaN(v) ? 0.7 : Math.max(0, Math.min(2, v));
                  this.edited = { ...this.edited, temperature: clamped };
                  this._onFormChange();
                }}
              ></hu-input>
              ${
                this.temperatureError
                  ? html`<span class="description description-error"
                      >${this.temperatureError}</span
                    >`
                  : nothing
              }
            </div>
          </hu-form-group>
        </hu-card>

        <hu-card glass>
          <hu-form-group title="Agent Settings" description="Workspace and paths">
            <div class="field">
              <label for="workspace_dir">Workspace directory</label>
              <hu-input
                id="workspace_dir"
                type="text"
                .value=${this.edited.workspace_dir ?? ""}
                placeholder="."
                @hu-input=${(e: CustomEvent<{ value: string }>) => {
                  this.edited = { ...this.edited, workspace_dir: e.detail.value };
                  this._onFormChange();
                }}
              ></hu-input>
            </div>
          </hu-form-group>
        </hu-card>
      </div>
      ${
        this.hasChanges()
          ? html`
              <div class="unsaved-banner">
                <span>You have unsaved changes</span>
                <div class="unsaved-banner-actions">
                  <hu-button variant="secondary" @click=${this.revert}>Revert</hu-button>
                  <hu-button variant="primary" ?disabled=${this.saving} @click=${() => this.save()}>
                    Save
                  </hu-button>
                </div>
              </div>
            `
          : nothing
      }
    `;
  }
}
