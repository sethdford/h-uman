import { LitElement, html, css } from "lit";
import { customElement, property } from "lit/decorators.js";
import "./hu-status-dot.js";

type AvatarSize = "sm" | "md" | "lg";
type AvatarStatus = "online" | "offline" | "busy" | "away" | "";

const SIZE_MAP: Record<AvatarSize, number> = {
  sm: 28,
  md: 36,
  lg: 48,
};

/** Simple hash of string to number for consistent color derivation */
function hashString(str: string): number {
  let h = 0;
  for (let i = 0; i < str.length; i++) {
    h = (h << 5) - h + str.charCodeAt(i);
    h |= 0;
  }
  return Math.abs(h);
}

/** Generate initials from name: "John Doe" -> "JD", "Alice" -> "A" */
function getInitials(name: string): string {
  const parts = name.trim().split(/\s+/).filter(Boolean);
  if (parts.length === 0) return "?";
  if (parts.length === 1) return parts[0].charAt(0).toUpperCase();
  return (parts[0].charAt(0) + parts[parts.length - 1].charAt(0)).toUpperCase();
}

/** Hue-based colors for initials background (accessible contrast) */
const INITIALS_HUES = [200, 280, 340, 40, 160];

function getInitialsBg(name: string): string {
  const idx = hashString(name) % INITIALS_HUES.length;
  const hue = INITIALS_HUES[idx];
  return `hsl(${hue}, 45%, 35%)`;
}

@customElement("hu-avatar")
export class ScAvatar extends LitElement {
  static override styles = css`
    :host {
      display: inline-flex;
      position: relative;
      font-family: var(--hu-font);
    }

    .avatar {
      border-radius: 50%;
      overflow: hidden;
      display: flex;
      align-items: center;
      justify-content: center;
      font-weight: var(--hu-weight-semibold);
      color: white;
      flex-shrink: 0;
    }

    .avatar img {
      width: 100%;
      height: 100%;
      object-fit: cover;
    }

    .status-dot-wrap {
      position: absolute;
      bottom: 0;
      right: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      border-radius: 50%;
      border: 2px solid var(--hu-bg-overlay);
      box-sizing: border-box;
    }

    .status-dot-wrap.size-sm {
      width: calc(var(--hu-space-sm) + var(--hu-space-2xs) * 2);
      height: calc(var(--hu-space-sm) + var(--hu-space-2xs) * 2);
    }

    .status-dot-wrap.size-md,
    .status-dot-wrap.size-lg {
      width: calc(0.625rem + var(--hu-space-2xs) * 2);
      height: calc(0.625rem + var(--hu-space-2xs) * 2);
    }
  `;

  @property() src = "";
  @property() name = "";
  @property({ type: String }) size: AvatarSize = "md";
  @property({ type: String }) status: AvatarStatus = "";

  private _imageError = false;

  override updated(changedProperties: Map<string, unknown>): void {
    if (changedProperties.has("src")) {
      this._imageError = false;
    }
  }

  private get _sizePx(): number {
    return SIZE_MAP[this.size];
  }

  private _onImageError = (): void => {
    this._imageError = true;
  };

  override render() {
    const sizePx = this._sizePx;
    const showImage = this.src && !this._imageError;
    const initials = getInitials(this.name || "?");
    const initialsBg = getInitialsBg(this.name || "0");

    return html`
      <div
        class="avatar"
        role="img"
        aria-label=${this.name || "Avatar"}
        style="
          width: ${sizePx}px;
          height: ${sizePx}px;
          font-size: ${sizePx * 0.4}px;
          background: ${showImage ? "transparent" : initialsBg};
        "
      >
        ${showImage
          ? html`<img
              src=${this.src}
              alt=${this.name || "User avatar"}
              @error=${this._onImageError}
            />`
          : html`<span>${initials}</span>`}
      </div>
      ${this.status
        ? html`
            <div class="status-dot-wrap size-${this.size}" aria-hidden="true">
              <hu-status-dot
                status=${this.status}
                size=${this.size === "lg" ? "md" : "sm"}
              ></hu-status-dot>
            </div>
          `
        : null}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-avatar": ScAvatar;
  }
}
