import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { icons } from "../icons.js";
import type { InstalledSkill, RegistrySkill } from "./hu-skill-card.js";
import "../components/hu-sheet.js";
import "../components/hu-badge.js";
import "../components/hu-button.js";
import "../components/hu-json-viewer.js";

export type SelectedSkill =
  | { source: "installed"; skill: InstalledSkill }
  | { source: "registry"; skill: RegistrySkill };

@customElement("hu-skill-detail")
export class ScSkillDetail extends LitElement {
  @property({ attribute: false }) skill: SelectedSkill | null = null;
  @property({ type: Array }) installedNames: string[] = [];
  @property({ type: Boolean }) actionLoading = false;

  static override styles = css`
    :host {
      display: block;
    }

    .detail-header {
      display: flex;
      align-items: center;
      gap: var(--hu-space-md);
      margin-bottom: var(--hu-space-xl);
    }

    .detail-icon {
      width: 2.5rem;
      height: 2.5rem;
      border-radius: var(--hu-radius-lg);
      background: var(--hu-bg-inset);
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--hu-text-muted);
      flex-shrink: 0;
    }

    .detail-icon svg {
      width: var(--hu-icon-md);
      height: var(--hu-icon-md);
    }

    .detail-name {
      font-size: var(--hu-text-xl);
      font-weight: var(--hu-weight-bold);
      color: var(--hu-text);
    }

    .detail-desc {
      font-size: var(--hu-text-base);
      color: var(--hu-text-muted);
      line-height: 1.6;
      margin-bottom: var(--hu-space-xl);
    }

    .detail-grid {
      display: grid;
      grid-template-columns: auto 1fr;
      gap: var(--hu-space-sm) var(--hu-space-xl);
      margin-bottom: var(--hu-space-2xl);
      font-size: var(--hu-text-sm);
    }

    .detail-label {
      color: var(--hu-text-muted);
      font-weight: var(--hu-weight-medium);
    }

    .detail-value {
      color: var(--hu-text);
      font-family: var(--hu-font-mono);
      word-break: break-all;
    }

    .detail-params {
      margin-bottom: var(--hu-space-2xl);
    }

    .detail-params-title {
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-semibold);
      color: var(--hu-text);
      margin-bottom: var(--hu-space-sm);
    }

    .detail-params-code {
      background: var(--hu-bg-inset);
      border-radius: var(--hu-radius);
      padding: var(--hu-space-md);
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      overflow-x: auto;
      white-space: pre-wrap;
    }

    .detail-actions {
      display: flex;
      gap: var(--hu-space-sm);
      flex-wrap: wrap;
    }
  `;

  private _parseParametersJson(raw: string): unknown {
    try {
      const trimmed = raw.trim();
      if (!trimmed) return null;
      return JSON.parse(trimmed);
    } catch {
      return null;
    }
  }

  private _renderParametersViewer(params: string) {
    const parsed = this._parseParametersJson(params);
    if (parsed !== null && typeof parsed === "object") {
      return html`<hu-json-viewer .data=${parsed} root-label="Parameters"></hu-json-viewer>`;
    }
    return html`<div class="detail-params-code">${params}</div>`;
  }

  private _onClose(): void {
    this.dispatchEvent(
      new CustomEvent("hu-close", {
        bubbles: true,
        composed: true,
      }),
    );
  }

  private _onToggle(): void {
    if (this.skill?.source === "installed") {
      this.dispatchEvent(
        new CustomEvent("skill-toggle", {
          detail: { skill: this.skill.skill },
          bubbles: true,
          composed: true,
        }),
      );
    }
  }

  private _onUninstall(): void {
    if (this.skill?.source === "installed") {
      this.dispatchEvent(
        new CustomEvent("skill-uninstall", {
          detail: { name: this.skill.skill.name },
          bubbles: true,
          composed: true,
        }),
      );
    }
  }

  private _onInstall(): void {
    if (this.skill?.source === "registry") {
      this.dispatchEvent(
        new CustomEvent("install", {
          detail: { skill: this.skill.skill },
          bubbles: true,
          composed: true,
        }),
      );
    }
  }

  override render() {
    const sel = this.skill;
    if (!sel) return nothing;

    const isInstalled = sel.source === "installed";
    const skill = sel.skill;
    const name = skill.name;
    const desc = skill.description ?? "No description";
    const inst = skill as InstalledSkill;
    const reg = skill as RegistrySkill;
    const alreadyInstalled = new Set(this.installedNames).has(name);

    return html`
      <hu-sheet .open=${true} size="md" @hu-close=${this._onClose}>
        <div class="detail-header">
          <div class="detail-icon">${icons.puzzle}</div>
          <div>
            <div class="detail-name">${name}</div>
            <hu-badge variant=${isInstalled ? "success" : "info"}
              >${isInstalled ? "Installed" : "Registry"}</hu-badge
            >
          </div>
        </div>
        <div class="detail-desc">${desc}</div>
        <div class="detail-grid">
          ${isInstalled
            ? html`<span class="detail-label">Status</span>
                <span class="detail-value">${inst.enabled ? "Enabled" : "Disabled"}</span>`
            : nothing}
          ${"version" in skill && reg.version
            ? html`<span class="detail-label">Version</span>
                <span class="detail-value">${reg.version}</span>`
            : nothing}
          ${"author" in skill && reg.author
            ? html`<span class="detail-label">Author</span>
                <span class="detail-value">${reg.author}</span>`
            : nothing}
          ${"tags" in skill && reg.tags
            ? html`<span class="detail-label">Tags</span>
                <span class="detail-value">${reg.tags}</span>`
            : nothing}
          ${"url" in skill && reg.url
            ? html`<span class="detail-label">URL</span>
                <span class="detail-value">${reg.url}</span>`
            : nothing}
        </div>
        ${isInstalled && inst.parameters
          ? html`<div class="detail-params">
              <div class="detail-params-title">Parameters</div>
              ${this._renderParametersViewer(inst.parameters)}
            </div>`
          : nothing}
        <div class="detail-actions">
          ${isInstalled
            ? html`
                <hu-button
                  variant=${inst.enabled ? "secondary" : "primary"}
                  ?loading=${this.actionLoading}
                  @click=${this._onToggle}
                  >${inst.enabled ? "Disable" : "Enable"}</hu-button
                >
                <hu-button
                  variant="destructive"
                  ?loading=${this.actionLoading}
                  @click=${this._onUninstall}
                  >Uninstall</hu-button
                >
              `
            : html`<hu-button
                variant="primary"
                ?loading=${this.actionLoading}
                ?disabled=${alreadyInstalled}
                @click=${this._onInstall}
                >${alreadyInstalled ? "Already Installed" : "Install"}</hu-button
              >`}
        </div>
      </hu-sheet>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-skill-detail": ScSkillDetail;
  }
}
