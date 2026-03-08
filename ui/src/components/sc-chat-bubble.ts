import { LitElement, html, css, nothing } from "lit";
import { customElement, property } from "lit/decorators.js";
import { renderMarkdown } from "../lib/markdown.js";
import "./sc-code-block.js";
import "./sc-toast.js";
import { ScToast } from "./sc-toast.js";

@customElement("sc-chat-bubble")
export class ScChatBubble extends LitElement {
  @property({ type: String }) content = "";
  @property({ type: String }) role: "user" | "assistant" = "assistant";
  @property({ type: Boolean }) streaming = false;
  @property({ type: Boolean }) completing = false;
  @property({ type: Boolean }) showTail = false;
  @property({ type: Boolean }) isLast = false;
  @property({ type: Boolean }) isFirst = false;

  static override styles = css`
    @keyframes sc-bubble-send {
      from {
        opacity: 0;
        transform: translateY(var(--sc-space-sm)) scale(0.97);
      }
      to {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }

    @keyframes sc-bubble-receive {
      from {
        opacity: 0;
        transform: translateY(var(--sc-space-xs)) scale(0.98);
      }
      to {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }

    @keyframes sc-cursor-glow {
      0%,
      100% {
        opacity: 1;
        box-shadow: 0 0 4px 1px color-mix(in srgb, var(--sc-accent) 40%, transparent);
      }
      50% {
        opacity: 0.4;
        box-shadow: 0 0 2px 0 color-mix(in srgb, var(--sc-accent) 15%, transparent);
      }
    }

    @keyframes sc-cursor-fade-out {
      from {
        opacity: 1;
        box-shadow: 0 0 8px 2px color-mix(in srgb, var(--sc-accent) 50%, transparent);
      }
      to {
        opacity: 0;
        box-shadow: 0 0 0 0 transparent;
      }
    }

    @keyframes sc-bubble-settle {
      0% {
        transform: scale(1);
      }
      40% {
        transform: scale(1.008);
      }
      100% {
        transform: scale(1);
      }
    }

    :host {
      display: block;
    }

    .bubble {
      position: relative;
      font-family: var(--sc-font);
      font-size: var(--sc-text-sm);
      line-height: var(--sc-leading-relaxed);
      padding: var(--sc-space-sm) var(--sc-space-md);
      box-shadow: var(--sc-shadow-sm);
    }

    .bubble.role-user {
      margin-left: auto;
      max-width: 75%;
      background: linear-gradient(
        135deg,
        color-mix(in srgb, var(--sc-accent) 85%, transparent),
        var(--sc-accent-hover)
      );
      color: var(--sc-on-accent-text, var(--sc-on-accent));
      border-radius: var(--sc-radius-xl) var(--sc-radius-xl) var(--sc-radius-sm) var(--sc-radius-xl);
      animation: sc-bubble-send var(--sc-duration-fast) var(--sc-ease-out) both;
    }

    .bubble.role-user::after {
      content: "";
      display: var(--tail-display, none);
      position: absolute;
      bottom: -1px;
      right: calc(-1 * var(--sc-space-sm));
      width: var(--sc-space-sm);
      height: calc(var(--sc-space-sm) + var(--sc-space-2xs));
      background: var(--sc-accent);
      clip-path: polygon(0 0, 100% 100%, 0 100%);
    }

    .bubble.role-user.show-tail::after {
      --tail-display: block;
    }

    .bubble.role-assistant {
      margin-right: auto;
      max-width: 85%;
      background: color-mix(in srgb, var(--sc-surface-container) 65%, transparent);
      backdrop-filter: blur(var(--sc-blur-md));
      -webkit-backdrop-filter: blur(var(--sc-blur-md));
      color: var(--sc-text);
      border: 1px solid color-mix(in srgb, var(--sc-color-white) 8%, transparent);
      border-radius: var(--sc-radius-xl) var(--sc-radius-xl) var(--sc-radius-xl) var(--sc-radius-sm);
      box-shadow: var(--sc-shadow-xs);
      animation: sc-bubble-receive var(--sc-duration-fast) var(--sc-ease-out) both;
    }

    .bubble.role-assistant::after {
      content: "";
      display: var(--tail-display, none);
      position: absolute;
      bottom: -1px;
      left: calc(-1 * var(--sc-space-sm));
      width: var(--sc-space-sm);
      height: calc(var(--sc-space-sm) + var(--sc-space-2xs));
      background: color-mix(in srgb, var(--sc-surface-container) 65%, transparent);
      clip-path: polygon(100% 0, 100% 100%, 0 100%);
    }

    .bubble.role-assistant.show-tail::after {
      --tail-display: block;
    }

    @media (prefers-reduced-transparency: reduce) {
      .bubble.role-assistant {
        backdrop-filter: none;
        -webkit-backdrop-filter: none;
        background: var(--sc-surface-container);
        border: 1px solid var(--sc-border-subtle);
      }
      .bubble.role-assistant::after {
        background: var(--sc-surface-container);
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .bubble.role-user,
      .bubble.role-assistant {
        animation: none;
      }
    }

    .content {
      position: relative;
      z-index: 1;
    }

    /* Assistant markdown content */
    .bubble.role-assistant .md-content {
      color: var(--sc-text);
    }

    .bubble.role-assistant .md-content a {
      color: var(--sc-accent);
      text-decoration: none;
    }

    .bubble.role-assistant .md-content a:hover {
      text-decoration: underline;
    }

    .bubble.role-assistant .md-content code.inline {
      font-family: var(--sc-font-mono);
      font-size: 0.9em;
      padding: 0.1em 0.35em;
      border-radius: var(--sc-radius-sm);
      background: var(--sc-bg-inset);
    }

    /* User bubble content */
    .bubble.role-user .md-content {
      color: var(--sc-on-accent);
    }

    .bubble.role-user .md-content a {
      color: var(--sc-on-accent);
      text-decoration: underline;
    }

    .bubble.role-user .md-content a:hover {
      text-decoration: underline;
    }

    .bubble.role-user .md-content code.inline {
      font-family: var(--sc-font-mono);
      font-size: 0.9em;
      padding: 0.1em 0.35em;
      border-radius: var(--sc-radius-sm);
      background: color-mix(in srgb, var(--sc-on-accent) 90%, transparent);
    }

    /* Shared markdown styles */
    .md-heading {
      font-weight: var(--sc-weight-semibold);
      margin: var(--sc-space-sm) 0 var(--sc-space-xs);
    }

    .md-heading:first-child {
      margin-top: 0;
    }

    .md-content h1.md-heading {
      font-size: var(--sc-text-2xl);
    }
    .md-content h2.md-heading {
      font-size: var(--sc-text-xl);
    }
    .md-content h3.md-heading {
      font-size: var(--sc-text-lg);
    }
    .md-content h4.md-heading,
    .md-content h5.md-heading,
    .md-content h6.md-heading {
      font-size: var(--sc-text-base);
    }

    .md-paragraph {
      margin: var(--sc-space-xs) 0;
    }

    .md-blockquote {
      border-left: 3px solid var(--sc-accent);
      padding-left: var(--sc-space-md);
      margin: var(--sc-space-sm) 0;
      color: var(--sc-text-muted);
      font-style: italic;
    }

    .bubble.role-user .md-blockquote {
      border-left-color: var(--sc-on-accent);
      color: color-mix(in srgb, var(--sc-on-accent) 85%, transparent);
    }

    .md-list {
      margin: var(--sc-space-sm) 0;
      padding-left: var(--sc-space-lg);
    }

    .md-list-item {
      margin-bottom: var(--sc-space-2xs);
    }

    .md-table {
      border-collapse: collapse;
      width: 100%;
      margin: var(--sc-space-sm) 0;
    }

    .md-table th,
    .md-table td {
      border: 1px solid var(--sc-border);
      padding: var(--sc-space-xs) var(--sc-space-sm);
      text-align: left;
    }

    .bubble.role-user .md-table th,
    .bubble.role-user .md-table td {
      border-color: color-mix(in srgb, var(--sc-on-accent) 30%, transparent);
    }

    .md-table th {
      background: var(--sc-bg-inset);
      font-weight: var(--sc-weight-semibold);
    }

    .bubble.role-user .md-table th {
      background: color-mix(in srgb, var(--sc-on-accent) 20%, transparent);
    }

    .md-hr {
      border: none;
      border-top: 1px solid var(--sc-border);
      margin: var(--sc-space-md) 0;
    }

    .bubble.role-user .md-hr {
      border-top-color: color-mix(in srgb, var(--sc-on-accent) 40%, transparent);
    }

    .md-content img {
      max-width: 100%;
      border-radius: var(--sc-radius-md);
    }

    .cursor {
      display: inline-block;
      width: 2px;
      height: 1.1em;
      background: var(--sc-accent);
      margin-left: var(--sc-space-2xs);
      vertical-align: text-bottom;
      border-radius: var(--sc-radius-xs);
      animation: sc-cursor-glow var(--sc-duration-slow) var(--sc-ease-in-out) infinite;
    }

    .cursor.completing {
      animation: sc-cursor-fade-out var(--sc-duration-normal) var(--sc-ease-out) forwards;
    }

    .bubble.role-user .cursor {
      background: var(--sc-on-accent);
    }

    .bubble.settling {
      animation: sc-bubble-settle var(--sc-duration-normal)
        var(--sc-ease-spring, cubic-bezier(0.34, 1.56, 0.64, 1));
    }

    @media (prefers-reduced-motion: reduce) {
      .cursor,
      .cursor.completing {
        animation: none;
      }
      .bubble.settling {
        animation: none;
      }
    }

    .footer {
      display: flex;
      align-items: center;
      justify-content: flex-end;
      gap: var(--sc-space-sm);
      margin-top: var(--sc-space-xs);
      min-height: 1.25em;
    }

    .bubble.role-user .footer {
      justify-content: flex-end;
    }

    .bubble.role-assistant .footer {
      justify-content: flex-start;
    }

    .meta-slot {
      opacity: 0;
      transition: opacity var(--sc-duration-fast) var(--sc-ease-out);
      font-size: var(--sc-text-xs);
      color: var(--sc-text-muted);
    }

    .bubble.role-user .meta-slot {
      color: color-mix(in srgb, var(--sc-on-accent) 80%, transparent);
    }

    .bubble:hover .meta-slot {
      opacity: 1;
    }

    @media (prefers-reduced-motion: reduce) {
      .meta-slot {
        transition: none;
      }
    }

    .bubble:focus-visible {
      outline: var(--sc-focus-ring-width) solid var(--sc-accent);
      outline-offset: var(--sc-focus-ring-offset);
    }
  `;

  private _copyCode(code: string): void {
    navigator.clipboard?.writeText(code).then(
      () => {
        ScToast.show({ message: "Copied to clipboard", variant: "success", duration: 2000 });
      },
      () => {
        ScToast.show({ message: "Failed to copy", variant: "error" });
      },
    );
  }

  override render() {
    const classes = [
      "bubble",
      `role-${this.role}`,
      this.showTail ? "show-tail" : "",
      this.completing ? "settling" : "",
    ]
      .filter(Boolean)
      .join(" ");
    const isUser = this.role === "user";
    const showCursor = this.streaming || this.completing;

    return html`
      <div
        class=${classes}
        role="article"
        aria-label=${isUser ? "Your message" : "Assistant message"}
        tabindex="0"
      >
        <div class="content">
          ${renderMarkdown(this.content, {
            onCopyCode: (code) => this._copyCode(code),
            streaming: this.streaming,
          })}
          ${showCursor
            ? html`<span
                class="cursor ${this.completing ? "completing" : ""}"
                aria-hidden="true"
              ></span>`
            : nothing}
        </div>
        <div class="footer">
          <slot name="status"></slot>
          <span class="meta-slot"><slot name="meta"></slot></span>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-chat-bubble": ScChatBubble;
  }
}
