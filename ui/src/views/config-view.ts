import { html, css, nothing, type TemplateResult } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";
import { ScToast } from "../components/sc-toast.js";
import { icons } from "../icons.js";
import "../components/sc-page-hero.js";
import "../components/sc-section-header.js";
import "../components/sc-card.js";
import "../components/sc-badge.js";
import "../components/sc-input.js";
import "../components/sc-button.js";
import "../components/sc-skeleton.js";
import "../components/sc-form-group.js";
import "../components/sc-combobox.js";
import "../components/sc-code-block.js";
import "../components/sc-empty-state.js";

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
  { value: "gemini-2.5-pro", label: "Gemini 2.5 Pro" },
  { value: "gemini-2.0-flash", label: "Gemini 2.0 Flash" },
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

@customElement("sc-config-view")
export class ScConfigView extends GatewayAwareLitElement {
  override autoRefreshInterval = 30_000;
  static override styles = css`
    :host {
      view-transition-name: view-config;
      display: block;
      max-width: 40rem;
      margin: 0 auto;
    }
    .header-actions {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: center;
    }
    .form {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xl);
    }
    .section {
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      overflow: hidden;
      background: var(--sc-bg-elevated);
    }
    .section + .section {
      margin-top: var(--sc-space-sm);
    }
    .section-header {
      padding: var(--sc-space-md) var(--sc-space-md);
      background: var(--sc-bg-elevated);
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: space-between;
      user-select: none;
      transition: background var(--sc-duration-fast);
    }
    .section-header:hover {
      background: var(--sc-bg-overlay);
    }
    .section-header .chevron {
      transition: transform var(--sc-duration-normal) var(--sc-ease-out);
      color: var(--sc-text-muted);
      display: flex;
    }
    .section-header .chevron svg {
      width: 0.875rem;
      height: 0.875rem;
    }
    .section-header.collapsed .chevron {
      transform: rotate(-90deg);
    }
    .section-content {
      padding: var(--sc-space-md) var(--sc-space-lg);
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md);
      max-height: 37.5rem;
      overflow: hidden;
      transition:
        max-height var(--sc-duration-slow) var(--sc-ease-out),
        padding var(--sc-duration-normal),
        opacity var(--sc-duration-normal);
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
      gap: var(--sc-space-xs);
    }
    .field label {
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
    }
    .field .description {
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      margin-top: var(--sc-space-2xs);
    }
    .field .description-error {
      color: var(--sc-error);
    }
    .raw-area {
      --sc-font: var(--sc-font-mono);
      --sc-text-base: var(--sc-text-sm);
      --sc-bg-elevated: var(--sc-bg-inset);
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
      padding: var(--sc-space-md) var(--sc-space-lg);
      background: var(--sc-bg-surface);
      border-top: 1px solid var(--sc-border);
      box-shadow: 0 calc(-1 * var(--sc-space-xs)) var(--sc-space-md) var(--sc-shadow-lg);
      z-index: 10;
    }
    .unsaved-banner-actions {
      display: flex;
      gap: var(--sc-space-sm);
    }
    @media (max-width: 30rem) /* --sc-breakpoint-sm */ {
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
  `;

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
    if (!this.gateway) return;
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
      this.errorMessage = e instanceof Error ? e.message : "Load failed";
      this.loadError = e instanceof Error ? e.message : "Load failed";
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
      console.warn("[config-view] failed to load schema, using defaults:", e);
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
      console.warn("[config-view] failed to load provider options, using defaults:", e);
    }
  }

  private hasChanges(): boolean {
    if (this.rawMode) {
      try {
        const parsed = JSON.parse(this.rawText) as Record<string, unknown>;
        const current = toRawConfig(this.config);
        return JSON.stringify(parsed) !== JSON.stringify(current);
      } catch (e) {
        console.warn("[config-view] malformed JSON in raw mode:", e);
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
      this.errorMessage = e instanceof Error ? e.message : "Save failed";
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
        console.warn("[config-view] failed to parse raw JSON, keeping previous state:", e);
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
      return html`<sc-empty-state
        .icon=${icons.warning}
        heading="Error"
        description=${this.loadError}
      ></sc-empty-state>`;
    if (this.rawMode) return this._renderRaw();
    return this._renderForm();
  }

  private _renderSkeleton(): TemplateResult {
    return html`
      <sc-page-hero role="region" aria-label="Configuration">
        <sc-section-header
          heading="Configuration"
          description="Manage your SeaClaw instance settings"
        ></sc-section-header>
      </sc-page-hero>
      <div class="form sc-stagger">
        <sc-skeleton variant="card" height="200px"></sc-skeleton>
        <sc-skeleton variant="card" height="200px"></sc-skeleton>
      </div>
    `;
  }

  private _renderSaveStatusBadge(): TemplateResult | typeof nothing {
    if (this.saving) return html`<sc-badge variant="warning">Saving...</sc-badge>`;
    if (this.saveStatus === "saved") return html`<sc-badge variant="success">Saved</sc-badge>`;
    if (this.hasChanges()) return html`<sc-badge variant="warning">Unsaved changes</sc-badge>`;
    if (this.saveStatus === "error")
      return html`<sc-badge variant="error">${this.errorMessage || "Error"}</sc-badge>`;
    return nothing;
  }

  private _renderRaw(): TemplateResult {
    return html`
      <sc-page-hero role="region" aria-label="Configuration">
        <sc-section-header
          heading="Configuration"
          description="Manage your SeaClaw instance settings"
        >
          ${this._renderSaveStatusBadge()}
          <div class="header-actions">
            <sc-button
              variant="ghost"
              size="sm"
              @click=${this.toggleRawMode}
              aria-label=${this.rawMode ? "Switch to form editor" : "Switch to raw JSON editor"}
            >
              ${this.rawMode ? "Form" : "Raw JSON"}
            </sc-button>
            <sc-button
              variant="primary"
              ?disabled=${!this.hasChanges()}
              @click=${() => this.save()}
              aria-label="Save configuration"
            >
              Save
            </sc-button>
          </div>
        </sc-section-header>
      </sc-page-hero>
      <sc-card glass>
        <div class="form">
          ${this.rawEditing
            ? html`
                <sc-textarea
                  class="raw-area"
                  .value=${this.rawText}
                  @sc-input=${(e: CustomEvent<{ value: string }>) => {
                    this.rawText = e.detail.value;
                    if (this.saveStatus === "saved") this.saveStatus = "idle";
                  }}
                  rows="12"
                  resize="vertical"
                ></sc-textarea>
                <sc-button variant="secondary" size="sm" @click=${() => (this.rawEditing = false)}>
                  Done editing
                </sc-button>
              `
            : html`
                <sc-code-block .code=${this.rawText} language="json"></sc-code-block>
                <sc-button variant="secondary" size="sm" @click=${() => (this.rawEditing = true)}>
                  Edit
                </sc-button>
              `}
        </div>
      </sc-card>
      ${this.hasChanges()
        ? html`
            <div class="unsaved-banner">
              <span>You have unsaved changes</span>
              <div class="unsaved-banner-actions">
                <sc-button variant="secondary" @click=${this.revert}>Revert</sc-button>
                <sc-button variant="primary" ?disabled=${this.saving} @click=${() => this.save()}>
                  Save
                </sc-button>
              </div>
            </div>
          `
        : nothing}
    `;
  }

  private _onFormChange(): void {
    if (this.saveStatus === "saved") this.saveStatus = "idle";
    this.validateTemperature();
  }

  private _renderForm(): TemplateResult {
    return html`
      <sc-page-hero role="region" aria-label="Configuration">
        <sc-section-header
          heading="Configuration"
          description="Manage your SeaClaw instance settings"
        >
          ${this._renderSaveStatusBadge()}
          <div class="header-actions">
            <sc-button
              variant="ghost"
              size="sm"
              @click=${this.toggleRawMode}
              aria-label=${this.rawMode ? "Switch to form editor" : "Switch to raw JSON editor"}
            >
              ${this.rawMode ? "Form" : "Raw JSON"}
            </sc-button>
            <sc-button
              variant="primary"
              ?disabled=${!this.hasChanges()}
              @click=${() => this.save()}
              aria-label="Save configuration"
            >
              Save
            </sc-button>
          </div>
        </sc-section-header>
      </sc-page-hero>
      <div class="form sc-stagger">
        <sc-card glass>
          <sc-form-group title="Provider Settings" description="Default AI provider and model">
            <sc-combobox
              label="Provider"
              .options=${this.providerOptions}
              .value=${this.edited.default_provider ?? ""}
              placeholder="Select provider"
              freeText
              @sc-combobox-change=${(e: CustomEvent<{ value: string }>) => {
                this.edited = { ...this.edited, default_provider: e.detail.value };
                this._onFormChange();
              }}
            ></sc-combobox>
            <sc-combobox
              label="Model"
              .options=${this.modelOptions}
              .value=${this.edited.default_model ?? ""}
              placeholder="Select or type model"
              freeText
              @sc-combobox-change=${(e: CustomEvent<{ value: string }>) => {
                this.edited = { ...this.edited, default_model: e.detail.value };
                this._onFormChange();
              }}
            ></sc-combobox>
          </sc-form-group>
        </sc-card>

        <sc-card glass>
          <sc-form-group title="Model Settings" description="Token and temperature limits">
            <div class="field">
              <label for="max_tokens">Max tokens</label>
              <sc-input
                id="max_tokens"
                type="number"
                .min=${0}
                .value=${String(this.edited.max_tokens ?? 0)}
                @sc-input=${(e: CustomEvent<{ value: string }>) => {
                  const v = parseInt(e.detail.value, 10);
                  this.edited = { ...this.edited, max_tokens: isNaN(v) ? 0 : v };
                  this._onFormChange();
                }}
              ></sc-input>
            </div>
            <div class="field">
              <label for="temperature">Temperature (0–2)</label>
              <sc-input
                id="temperature"
                type="number"
                .min=${0}
                .max=${2}
                .step=${0.1}
                .value=${String(this.edited.temperature ?? 0.7)}
                .error=${this.temperatureError}
                @sc-input=${(e: CustomEvent<{ value: string }>) => {
                  const v = parseFloat(e.detail.value);
                  const clamped = isNaN(v) ? 0.7 : Math.max(0, Math.min(2, v));
                  this.edited = { ...this.edited, temperature: clamped };
                  this._onFormChange();
                }}
              ></sc-input>
              ${this.temperatureError
                ? html`<span class="description description-error">${this.temperatureError}</span>`
                : nothing}
            </div>
          </sc-form-group>
        </sc-card>

        <sc-card glass>
          <sc-form-group title="Agent Settings" description="Workspace and paths">
            <div class="field">
              <label for="workspace_dir">Workspace directory</label>
              <sc-input
                id="workspace_dir"
                type="text"
                .value=${this.edited.workspace_dir ?? ""}
                placeholder="."
                @sc-input=${(e: CustomEvent<{ value: string }>) => {
                  this.edited = { ...this.edited, workspace_dir: e.detail.value };
                  this._onFormChange();
                }}
              ></sc-input>
            </div>
          </sc-form-group>
        </sc-card>
      </div>
      ${this.hasChanges()
        ? html`
            <div class="unsaved-banner">
              <span>You have unsaved changes</span>
              <div class="unsaved-banner-actions">
                <sc-button variant="secondary" @click=${this.revert}>Revert</sc-button>
                <sc-button variant="primary" ?disabled=${this.saving} @click=${() => this.save()}>
                  Save
                </sc-button>
              </div>
            </div>
          `
        : nothing}
    `;
  }
}
