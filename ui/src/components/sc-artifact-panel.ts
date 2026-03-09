import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import "./sc-artifact-viewer.js";
import "./sc-button.js";
import "./sc-toast.js";
import { ScToast } from "./sc-toast.js";
import { icons } from "../icons.js";
import type { ArtifactData } from "../controllers/chat-controller.js";

type PanelState = "closed" | "opening" | "open" | "closing";

const TYPE_TO_ICON: Record<ArtifactData["type"], keyof typeof icons> = {
  code: "code",
  document: "file-text",
  html: "monitor",
  diagram: "chart-line",
};

@customElement("sc-artifact-panel")
export class ScArtifactPanel extends LitElement {
  @property({ type: Object }) artifact: ArtifactData | null = null;
  @property({ type: Boolean }) open = false;

  @state() private _panelState: PanelState = "closed";
  @state() private _currentVersionIndex = 0;

  static override styles = css`
    :host {
      display: block;
      position: relative;
      width: 50%;
      min-width: 0;
      flex-shrink: 0;
    }
    .panel-wrap {
      position: absolute;
      top: 0;
      right: 0;
      bottom: 0;
      width: 100%;
      display: flex;
      flex-direction: column;
      background: var(--sc-surface-container);
      border-left: 1px solid var(--sc-border-subtle);
      box-shadow: var(--sc-shadow-lg);
      transform: translateX(100%);
      transition: transform var(--sc-duration-slow)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1));
      z-index: 10;
    }
    .panel-wrap.visible {
      transform: translateX(0);
    }
    .panel-wrap.hidden {
      display: none;
    }
    .header {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      background: color-mix(in srgb, var(--sc-border) 15%, var(--sc-bg-surface));
      border-bottom: 1px solid var(--sc-border-subtle);
      flex-shrink: 0;
    }
    .header-icon {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-icon-md);
      height: var(--sc-icon-md);
      color: var(--sc-text-muted);
    }
    .header-icon svg {
      width: 100%;
      height: 100%;
    }
    .header-title {
      flex: 1;
      min-width: 0;
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      font-weight: var(--sc-weight-medium);
      color: var(--sc-text);
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .version-nav {
      display: flex;
      align-items: center;
      gap: var(--sc-space-2xs);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
      font-family: var(--sc-font);
    }
    .version-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-icon-sm);
      height: var(--sc-icon-sm);
      padding: 0;
      background: transparent;
      border: 1px solid var(--sc-border-subtle);
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
    }
    .version-btn:hover:not(:disabled) {
      color: var(--sc-accent);
      border-color: var(--sc-accent);
    }
    .version-btn:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    .version-btn svg {
      width: 0.75rem;
      height: 0.75rem;
    }
    .close-btn {
      display: flex;
      align-items: center;
      justify-content: center;
      width: var(--sc-icon-md);
      height: var(--sc-icon-md);
      padding: 0;
      background: transparent;
      border: none;
      border-radius: var(--sc-radius-sm);
      color: var(--sc-text-muted);
      cursor: pointer;
    }
    .close-btn:hover {
      color: var(--sc-text);
      background: var(--sc-hover-overlay);
    }
    .close-btn:focus-visible {
      outline: 2px solid var(--sc-accent);
      outline-offset: 2px;
    }
    .close-btn svg {
      width: 1rem;
      height: 1rem;
    }
    .body {
      flex: 1;
      min-height: 0;
      overflow: hidden;
    }
    .footer {
      display: flex;
      align-items: center;
      gap: var(--sc-space-sm);
      padding: var(--sc-space-sm) var(--sc-space-md);
      border-top: 1px solid var(--sc-border-subtle);
      flex-shrink: 0;
    }
    @media (prefers-reduced-motion: reduce) {
      .panel-wrap {
        transition: none;
      }
    }
  `;

  override updated(changed: Map<string, unknown>): void {
    if (changed.has("open") || changed.has("artifact")) {
      this._updatePanelState();
    }
  }

  private _updatePanelState(): void {
    if (this.open && this.artifact) {
      if (this._panelState === "closed") {
        this._panelState = "opening";
        this._currentVersionIndex = Math.max(0, this.artifact.versions.length - 1);
      } else {
        this._panelState = "open";
      }
    } else if (!this.open) {
      if (this._panelState === "open" || this._panelState === "opening") {
        this._panelState = "closing";
        const duration = 200;
        setTimeout(() => {
          this._panelState = "closed";
          this.requestUpdate();
        }, duration);
      }
    }
  }

  private _onClose(): void {
    this.dispatchEvent(new CustomEvent("sc-artifact-close", { bubbles: true, composed: true }));
  }

  private _onCopy(): void {
    const content = this._getCurrentContent();
    if (!content) return;
    navigator.clipboard
      ?.writeText(content)
      .then(() => {
        ScToast.show({ message: "Copied to clipboard", variant: "success", duration: 2000 });
      })
      .catch(() => {
        ScToast.show({ message: "Failed to copy", variant: "error" });
      });
  }

  private _onDownload(): void {
    const content = this._getCurrentContent();
    if (!content || !this.artifact) return;
    const ext =
      this.artifact.type === "code" && this.artifact.language
        ? _langToExt(this.artifact.language)
        : this.artifact.type === "document"
          ? "md"
          : this.artifact.type === "html"
            ? "html"
            : "txt";
    const blob = new Blob([content], { type: "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `${this.artifact.title || "artifact"}.${ext}`;
    a.click();
    URL.revokeObjectURL(url);
    ScToast.show({ message: "Download started", variant: "success", duration: 2000 });
  }

  private _getCurrentContent(): string {
    if (!this.artifact) return "";
    const versions = this.artifact.versions;
    if (
      versions.length > 0 &&
      this._currentVersionIndex >= 0 &&
      this._currentVersionIndex < versions.length
    ) {
      return versions[this._currentVersionIndex]!.content;
    }
    return this.artifact.content;
  }

  private _prevVersion(): void {
    if (this._currentVersionIndex > 0) {
      this._currentVersionIndex--;
      this.requestUpdate();
    }
  }

  private _nextVersion(): void {
    if (this.artifact && this._currentVersionIndex < this.artifact.versions.length - 1) {
      this._currentVersionIndex++;
      this.requestUpdate();
    }
  }

  override render() {
    const a = this.artifact;
    const isVisible = this._panelState === "opening" || this._panelState === "open";
    const isHidden = this._panelState === "closed";
    const versionCount = a?.versions.length ?? 0;
    const canPrev = this._currentVersionIndex > 0;
    const canNext = a && this._currentVersionIndex < versionCount - 1;

    if (!a) {
      return nothing;
    }

    const content = this._getCurrentContent();
    const iconKey = TYPE_TO_ICON[a.type] ?? "file-text";

    return html`
      <div
        class="panel-wrap ${isVisible ? "visible" : ""} ${isHidden ? "hidden" : ""}"
        role="dialog"
        aria-label="Artifact panel"
        aria-modal="true"
      >
        <header class="header">
          <span class="header-icon" aria-hidden="true"
            >${icons[iconKey] ?? icons["file-text"]}</span
          >
          <span class="header-title">${a.title}</span>
          ${versionCount > 1
            ? html`
                <div class="version-nav">
                  <button
                    type="button"
                    class="version-btn"
                    ?disabled=${!canPrev}
                    aria-label="Previous version"
                    @click=${this._prevVersion}
                  >
                    ${icons["caret-left"]}
                  </button>
                  <span>${this._currentVersionIndex + 1} / ${versionCount}</span>
                  <button
                    type="button"
                    class="version-btn"
                    ?disabled=${!canNext}
                    aria-label="Next version"
                    @click=${this._nextVersion}
                  >
                    ${icons["caret-right"]}
                  </button>
                </div>
              `
            : nothing}
          <button
            type="button"
            class="close-btn"
            aria-label="Close artifact panel"
            @click=${this._onClose}
          >
            ${icons.x}
          </button>
        </header>
        <div class="body">
          <sc-artifact-viewer
            .type=${a.type}
            .content=${content}
            .language=${a.language ?? ""}
          ></sc-artifact-viewer>
        </div>
        <footer class="footer">
          <sc-button variant="secondary" size="sm" @click=${this._onCopy}>
            ${icons.copy} Copy
          </sc-button>
          <sc-button variant="secondary" size="sm" @click=${this._onDownload}> Download </sc-button>
        </footer>
      </div>
    `;
  }
}

function _langToExt(lang: string): string {
  const map: Record<string, string> = {
    javascript: "js",
    typescript: "ts",
    python: "py",
    bash: "sh",
    shell: "sh",
    html: "html",
    css: "css",
    json: "json",
    yaml: "yml",
    markdown: "md",
    c: "c",
    rust: "rs",
    go: "go",
    java: "java",
    ruby: "rb",
    php: "php",
    swift: "swift",
    kotlin: "kt",
    zig: "zig",
  };
  return map[lang.toLowerCase()] ?? "txt";
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-artifact-panel": ScArtifactPanel;
  }
}
