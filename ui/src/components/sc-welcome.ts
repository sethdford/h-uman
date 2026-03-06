import { LitElement, html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./sc-button.js";
import "./sc-card.js";
import "./sc-animated-icon.js";

const ONBOARDED_KEY = "sc-onboarded";

interface OnboardStep {
  key: string;
  icon: string;
  title: string;
  description: string;
  action: string;
  done: boolean;
}

@customElement("sc-welcome")
export class ScWelcome extends LitElement {
  static override styles = css`
    :host {
      display: block;
    }
    .welcome {
      animation: welcome-enter 600ms cubic-bezier(0.22, 1, 0.36, 1) both;
    }
    @keyframes welcome-enter {
      from {
        opacity: 0;
        transform: translateY(12px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }
    .greeting {
      margin-bottom: var(--sc-space-2xl, 2rem);
    }
    .greeting h2 {
      font-size: clamp(1.75rem, 3vw, 2.25rem);
      font-weight: var(--sc-weight-bold, 700);
      letter-spacing: -0.035em;
      color: var(--sc-text);
      margin: 0 0 var(--sc-space-sm, 0.5rem);
      line-height: 1.1;
    }
    .greeting p {
      font-size: var(--sc-text-base);
      color: var(--sc-text-muted);
      margin: 0;
      line-height: var(--sc-leading-relaxed);
    }
    .steps {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
      gap: var(--sc-space-lg, 1.5rem);
      margin-bottom: var(--sc-space-2xl, 2rem);
    }
    .step-card {
      display: flex;
      flex-direction: column;
      gap: var(--sc-space-md, 1rem);
      position: relative;
      overflow: hidden;
      cursor: pointer;
    }
    .step-card:focus-visible {
      outline: var(--sc-focus-ring-width, 2px) solid var(--sc-focus-ring);
      outline-offset: 2px;
      border-radius: var(--sc-radius-xl, 16px);
    }
    /* Staggered entrance for cards */
    .step-card:nth-child(1) {
      animation-delay: 80ms;
    }
    .step-card:nth-child(2) {
      animation-delay: 140ms;
    }
    .step-card:nth-child(3) {
      animation-delay: 200ms;
    }
    .step-card:nth-child(4) {
      animation-delay: 260ms;
    }

    .step-icon-wrap {
      width: 2.75rem;
      height: 2.75rem;
      display: flex;
      align-items: center;
      justify-content: center;
      border-radius: var(--sc-radius-lg, 12px);
      background: color-mix(in srgb, var(--sc-accent) 10%, transparent);
      transition:
        background 280ms ease,
        transform 280ms cubic-bezier(0.22, 1, 0.36, 1);
    }
    .step-card:hover .step-icon-wrap {
      background: color-mix(in srgb, var(--sc-accent) 18%, transparent);
      transform: scale(1.05);
    }
    .step-icon-wrap.done {
      background: color-mix(in srgb, var(--sc-success) 12%, transparent);
    }
    .step-icon {
      width: 1.25rem;
      height: 1.25rem;
      color: var(--sc-accent);
    }
    .step-icon svg {
      width: 100%;
      height: 100%;
    }
    .step-icon.done {
      color: var(--sc-success);
    }
    .step-title {
      font-size: var(--sc-text-base);
      font-weight: var(--sc-weight-semibold, 600);
      color: var(--sc-text);
      letter-spacing: -0.01em;
    }
    .step-desc {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      line-height: var(--sc-leading-relaxed);
      flex: 1;
    }
    .step-progress {
      position: absolute;
      bottom: 0;
      left: 0;
      right: 0;
      height: 2px;
      border-radius: 0 0 var(--sc-radius-xl, 16px) var(--sc-radius-xl, 16px);
    }
    .step-progress.done {
      background: linear-gradient(
        90deg,
        var(--sc-success),
        color-mix(in srgb, var(--sc-success) 40%, transparent)
      );
    }
    .step-progress.pending {
      background: color-mix(in srgb, var(--sc-border) 30%, transparent);
    }
    .dismiss {
      text-align: center;
    }
    .dismiss-link {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-faint, var(--sc-text-muted));
      background: none;
      border: none;
      cursor: pointer;
      text-decoration: none;
      font-family: var(--sc-font);
      padding: var(--sc-space-sm);
      transition: color 200ms ease;
      letter-spacing: 0.01em;
    }
    .dismiss-link:hover {
      color: var(--sc-text);
      text-decoration: underline;
      text-underline-offset: 3px;
    }
    .dismiss-link:focus-visible {
      outline: var(--sc-focus-ring-width, 2px) solid var(--sc-focus-ring);
      outline-offset: 2px;
      border-radius: var(--sc-radius-sm, 4px);
    }
    .celebration {
      text-align: center;
      padding: var(--sc-space-2xl, 2rem);
    }
    .celebration .check-icon {
      width: 3.5rem;
      height: 3.5rem;
      color: var(--sc-success);
      margin: 0 auto var(--sc-space-lg, 1.5rem);
      animation: sc-icon-pop 500ms cubic-bezier(0.22, 1, 0.36, 1);
    }
    .celebration .check-icon svg {
      width: 100%;
      height: 100%;
    }
    @keyframes sc-icon-pop {
      0% {
        transform: scale(0);
        opacity: 0;
      }
      40% {
        transform: scale(1.15);
        opacity: 1;
      }
      70% {
        transform: scale(0.95);
      }
      100% {
        transform: scale(1);
        opacity: 1;
      }
    }
    .celebration h3 {
      font-size: var(--sc-text-xl, 1.25rem);
      font-weight: var(--sc-weight-bold, 700);
      letter-spacing: -0.02em;
      margin: 0 0 var(--sc-space-xs);
    }
    .celebration p {
      font-size: var(--sc-text-sm);
      color: var(--sc-text-muted);
      margin: 0;
      max-width: 32ch;
      margin-inline: auto;
      line-height: var(--sc-leading-relaxed);
    }
    @media (prefers-reduced-motion: reduce) {
      .welcome,
      .hero-icon-wrap,
      .step-card {
        animation: none !important;
      }
    }
  `;

  @state() private steps: OnboardStep[] = [
    {
      key: "connect",
      icon: "zap",
      title: "Connect Gateway",
      description: "SeaClaw is connecting to your local gateway. This happens automatically.",
      action: "overview",
      done: false,
    },
    {
      key: "health",
      icon: "shield",
      title: "Verify Health",
      description: "Check that all subsystems are operational and responding.",
      action: "overview",
      done: false,
    },
    {
      key: "channel",
      icon: "radio",
      title: "Configure a Channel",
      description: "Set up Telegram, Discord, Slack, or any messaging channel.",
      action: "channels",
      done: false,
    },
    {
      key: "chat",
      icon: "message-square",
      title: "Start a Conversation",
      description: "Send your first message and see SeaClaw in action.",
      action: "chat",
      done: false,
    },
  ];
  @state() private dismissed = false;
  @state() private allDone = false;

  get visible(): boolean {
    if (this.dismissed) return false;
    return localStorage.getItem(ONBOARDED_KEY) !== "true";
  }

  markStep(key: string): void {
    this.steps = this.steps.map((s) => (s.key === key ? { ...s, done: true } : s));
    if (this.steps.every((s) => s.done)) {
      this.allDone = true;
      setTimeout(() => {
        localStorage.setItem(ONBOARDED_KEY, "true");
      }, 3000);
    }
  }

  private _dismiss(): void {
    this.dismissed = true;
    localStorage.setItem(ONBOARDED_KEY, "true");
  }

  private _navigate(tab: string): void {
    this.dispatchEvent(new CustomEvent("navigate", { detail: tab, bubbles: true, composed: true }));
  }

  override render() {
    if (!this.visible) return nothing;

    if (this.allDone) {
      return html`
        <sc-card elevated class="celebration">
          <div class="check-icon">${icons.check}</div>
          <h3>You're all set!</h3>
          <p>SeaClaw is configured and ready. Explore the dashboard to manage your AI assistant.</p>
        </sc-card>
      `;
    }

    const done = this.steps.filter((s) => s.done).length;
    const total = this.steps.length;

    return html`
      <div class="welcome" role="region" aria-label="Welcome and setup guide">
        <div class="greeting">
          <h2>Welcome to SeaClaw</h2>
          <p>Let's get you set up. ${done}/${total} steps complete.</p>
        </div>
        <div class="steps">
          ${this.steps.map(
            (step) => html`
              <sc-card
                hoverable
                accent
                class="step-card"
                role="button"
                tabindex="0"
                aria-label="${step.done
                  ? `${step.title} — complete`
                  : step.title}: ${step.description}"
                @click=${() => this._navigate(step.action)}
                @keydown=${(e: KeyboardEvent) => {
                  if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    this._navigate(step.action);
                  }
                }}
              >
                <div class="step-icon-wrap ${step.done ? "done" : ""}">
                  <div class="step-icon ${step.done ? "done" : ""}">
                    ${step.done ? icons.check : icons[step.icon]}
                  </div>
                </div>
                <div class="step-title">${step.title}</div>
                <div class="step-desc">${step.description}</div>
                ${!step.done
                  ? html`<sc-button variant="secondary" size="sm">Go</sc-button>`
                  : nothing}
                <div class="step-progress ${step.done ? "done" : "pending"}"></div>
              </sc-card>
            `,
          )}
        </div>
        <div class="dismiss">
          <button class="dismiss-link" @click=${this._dismiss}>
            Skip setup — I know what I'm doing
          </button>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "sc-welcome": ScWelcome;
  }
}
