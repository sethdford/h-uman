import { html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { GatewayAwareLitElement } from "../gateway-aware.js";

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
  static override styles = css`
    :host {
      display: block;
      max-width: 640px;
      margin: 0 auto;
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: var(--sc-space-xl);
      gap: var(--sc-space-md);
      flex-wrap: wrap;
    }
    .status {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
    }
    .status.saved {
      color: var(--sc-success);
    }
    .status.error {
      color: var(--sc-error);
    }
    .status.unsaved {
      color: var(--sc-accent);
    }
    .status.unsaved::before {
      content: "";
      display: inline-block;
      width: 6px;
      height: 6px;
      border-radius: 50%;
      background: var(--sc-accent);
      margin-right: var(--sc-space-xs);
      vertical-align: middle;
    }
    .header-actions {
      display: flex;
      gap: var(--sc-space-sm);
      align-items: center;
    }
    .toggle-btn {
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg-elevated);
      color: var(--sc-text);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      font-size: var(--sc-text-sm);
      cursor: pointer;
    }
    .toggle-btn:hover {
      background: var(--sc-border);
    }
    .save-btn {
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-accent);
      color: var(--sc-bg);
      border: none;
      border-radius: var(--sc-radius);
      font-weight: var(--sc-weight-medium);
      cursor: pointer;
      font-size: var(--sc-text-base);
    }
    .save-btn:hover:not(:disabled) {
      background: var(--sc-accent-hover);
    }
    .save-btn:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    .form {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-xl);
      padding: var(--sc-space-lg);
      background: var(--sc-bg-surface);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius-lg);
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
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }
    .section-header.collapsed .chevron {
      transform: rotate(-90deg);
    }
    .section-content {
      padding: var(--sc-space-md) var(--sc-space-lg);
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md);
      max-height: 600px;
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
    .field input {
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-size: var(--sc-text-base);
      transition:
        border-color var(--sc-duration-fast),
        box-shadow var(--sc-duration-fast);
    }
    .field input:hover:not(:focus) {
      border-color: var(--sc-text-muted);
    }
    .field input:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle);
    }
    .field input[type="number"] {
      width: 8rem;
    }
    .raw-area {
      min-height: 280px;
      padding: var(--sc-space-md) var(--sc-space-md);
      background: var(--sc-bg);
      border: 1px solid var(--sc-border);
      border-radius: var(--sc-radius);
      color: var(--sc-text);
      font-family: var(--sc-font-mono);
      font-size: var(--sc-text-sm);
      resize: vertical;
      transition:
        border-color var(--sc-duration-fast),
        box-shadow var(--sc-duration-fast);
    }
    .raw-area:hover:not(:focus) {
      border-color: var(--sc-text-muted);
    }
    .raw-area:focus {
      outline: none;
      border-color: var(--sc-accent);
      box-shadow: 0 0 0 3px var(--sc-accent-subtle);
    }
    @media (max-width: 768px) {
      .header {
        flex-wrap: wrap;
      }
    }
    @media (max-width: 480px) {
      :host {
        max-width: 100%;
      }
      .field input[type="number"] {
        width: 100%;
      }
    }
  `;

  @state() private config: ConfigData = {};
  @state() private edited: ConfigData = {};
  @state() private schema: ConfigSchema = {};
  @state() private rawMode = false;
  @state() private rawText = "";
  @state() private sectionCollapsed = false;
  @state() private saveStatus: SaveStatus = "idle";
  @state() private errorMessage = "";
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
    await Promise.all([this.loadConfig(), this.loadSchema()]);
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
    } catch {
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

  private hasChanges(): boolean {
    if (this.rawMode) {
      try {
        const parsed = JSON.parse(this.rawText) as Record<string, unknown>;
        const current = toRawConfig(this.config);
        return JSON.stringify(parsed) !== JSON.stringify(current);
      } catch {
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

  private async save(): Promise<void> {
    if (!this.gateway) return;
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
        this.saveStatus = "saved";
        setTimeout(() => {
          if (this.saveStatus === "saved") this.saveStatus = "idle";
        }, 2000);
      } else {
        this.saveStatus = "error";
        this.errorMessage = "Save rejected";
      }
    } catch (e) {
      this.saveStatus = "error";
      this.errorMessage = e instanceof Error ? e.message : "Save failed";
    }
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
      } catch {
        /* invalid JSON, keep edited */
      }
    } else {
      this.rawText = JSON.stringify(toRawConfig(this.edited), null, 2);
    }
    this.rawMode = !this.rawMode;
  }

  override render() {
    const statusText =
      this.saveStatus === "saved"
        ? "Saved"
        : this.saveStatus === "error"
          ? this.errorMessage || "Error"
          : this.hasChanges()
            ? "Unsaved changes"
            : "";

    const props = this.schema?.properties ?? {};
    const order = [
      "workspace_dir",
      "default_provider",
      "default_model",
      "max_tokens",
      "temperature",
    ];
    const orderedKeys = order.filter((k) => k in props);
    const extraKeys = Object.keys(props).filter((k) => !order.includes(k));
    const fieldKeys = [...orderedKeys, ...extraKeys];

    return html`
      <div class="header">
        <span class="status ${this.saveStatus}">${statusText}</span>
        <div class="header-actions">
          <button class="toggle-btn" @click=${this.toggleRawMode}>
            ${this.rawMode ? "Form" : "Raw JSON"}
          </button>
          <button class="save-btn" ?disabled=${!this.hasChanges()} @click=${() => this.save()}>
            Save
          </button>
        </div>
      </div>
      ${this.rawMode
        ? html`
            <div class="form">
              <textarea
                class="raw-area"
                .value=${this.rawText}
                @input=${(e: Event) => {
                  this.rawText = (e.target as HTMLTextAreaElement).value;
                  if (this.saveStatus === "saved") this.saveStatus = "idle";
                }}
                spellcheck="false"
              ></textarea>
            </div>
          `
        : html`
            <div class="form">
              <div class="section ${this.sectionCollapsed ? "collapsed" : ""}">
                <div
                  class="section-header ${this.sectionCollapsed ? "collapsed" : ""}"
                  @click=${() => (this.sectionCollapsed = !this.sectionCollapsed)}
                >
                  <span>General</span>
                  <span class="chevron">▼</span>
                </div>
                <div class="section-content">
                  ${fieldKeys.map((key) => {
                    const prop = props[key] as SchemaProperty | undefined;
                    const desc = prop?.description ?? "";
                    const val = this.edited[key as keyof ConfigData];
                    const inputType =
                      prop?.type === "integer" || prop?.type === "number" ? "number" : "text";

                    return html`
                      <div class="field">
                        <label for="${key}">${key.replace(/_/g, " ")}</label>
                        ${desc ? html`<div class="description">${desc}</div>` : nothing}
                        <input
                          id="${key}"
                          type="${inputType}"
                          min=${inputType === "number" ? 0 : undefined}
                          max=${key === "temperature" ? 2 : undefined}
                          step=${key === "temperature"
                            ? 0.1
                            : inputType === "number"
                              ? 1
                              : undefined}
                          .value=${String(val ?? "")}
                          @input=${(e: Event) => {
                            const t = e.target as HTMLInputElement;
                            const v =
                              inputType === "number"
                                ? key === "temperature"
                                  ? parseFloat(t.value)
                                  : parseInt(t.value, 10)
                                : t.value;
                            const parsed =
                              inputType === "number"
                                ? isNaN(v as number)
                                  ? key === "temperature"
                                    ? 0.7
                                    : 0
                                  : key === "temperature"
                                    ? Math.max(0, Math.min(2, v as number))
                                    : v
                                : v;
                            this.edited = {
                              ...this.edited,
                              [key]: parsed,
                            };
                            if (this.saveStatus === "saved") this.saveStatus = "idle";
                          }}
                        />
                      </div>
                    `;
                  })}
                </div>
              </div>
            </div>
          `}
    `;
  }
}
