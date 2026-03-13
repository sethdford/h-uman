import { LitElement, html, css, nothing } from "lit";
import { customElement, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./hu-button.js";
import "./hu-card.js";
import "./hu-animated-icon.js";

const ONBOARDED_KEY = "hu-onboarded";

interface OnboardStep {
  key: string;
  icon: string;
  title: string;
  description: string;
  action: string;
  done: boolean;
}

@customElement("hu-welcome")
export class ScWelcome extends LitElement {
  static override styles = css`
    :host {
      display: block;
    }
    .welcome {
      animation: hu-welcome-enter var(--hu-duration-slow) var(--hu-ease-out) both;
    }
    @keyframes hu-welcome-enter {
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
      margin-bottom: var(--hu-space-2xl, 2rem);
    }
    .greeting h2 {
      font-size: clamp(1.75rem, 3vw, 2.25rem);
      font-weight: var(--hu-weight-bold, 700);
      letter-spacing: -0.035em;
      color: var(--hu-text);
      margin: 0 0 var(--hu-space-sm, 0.5rem);
      line-height: 1.1;
    }
    .greeting p {
      font-size: var(--hu-text-base);
      color: var(--hu-text-muted);
      margin: 0;
      line-height: var(--hu-leading-relaxed);
    }
    .steps {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
      gap: var(--hu-space-lg, 1.5rem);
      margin-bottom: var(--hu-space-2xl, 2rem);
    }
    .step-card {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md, 1rem);
      position: relative;
      overflow: hidden;
      cursor: pointer;
    }
    .step-card:focus-visible {
      outline: var(--hu-focus-ring-width, 2px) solid var(--hu-focus-ring);
      outline-offset: 2px;
      border-radius: var(--hu-radius-xl, 16px);
    }
    /* Staggered entrance for cards */
    .step-card:nth-child(1) {
      animation-delay: calc(1 * var(--hu-stagger-delay));
    }
    .step-card:nth-child(2) {
      animation-delay: calc(2 * var(--hu-stagger-delay));
    }
    .step-card:nth-child(3) {
      animation-delay: calc(3 * var(--hu-stagger-delay));
    }
    .step-card:nth-child(4) {
      animation-delay: calc(4 * var(--hu-stagger-delay));
    }

    .step-icon-wrap {
      width: 2.75rem;
      height: 2.75rem;
      display: flex;
      align-items: center;
      justify-content: center;
      border-radius: var(--hu-radius-lg, 12px);
      background: color-mix(in srgb, var(--hu-accent) 10%, transparent);
      transition:
        background var(--hu-duration-normal) var(--hu-ease-out),
        transform var(--hu-duration-normal) var(--hu-ease-out);
    }
    .step-card:hover .step-icon-wrap {
      background: color-mix(in srgb, var(--hu-accent) 18%, transparent);
      transform: scale(1.05);
    }
    .step-icon-wrap.done {
      background: color-mix(in srgb, var(--hu-success) 12%, transparent);
    }
    .step-icon {
      width: 1.25rem;
      height: 1.25rem;
      color: var(--hu-accent);
    }
    .step-icon svg {
      width: 100%;
      height: 100%;
    }
    .step-icon.done {
      color: var(--hu-success);
    }
    .step-title {
      font-size: var(--hu-text-base);
      font-weight: var(--hu-weight-semibold, 600);
      color: var(--hu-text);
      letter-spacing: -0.01em;
    }
    .step-desc {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      line-height: var(--hu-leading-relaxed);
      flex: 1;
    }
    .step-progress {
      position: absolute;
      bottom: 0;
      left: 0;
      right: 0;
      height: 2px;
      border-radius: 0 0 var(--hu-radius-xl, 16px) var(--hu-radius-xl, 16px);
    }
    .step-progress.done {
      background: linear-gradient(
        90deg,
        var(--hu-success),
        color-mix(in srgb, var(--hu-success) 40%, transparent)
      );
    }
    .step-progress.pending {
      background: color-mix(in srgb, var(--hu-border) 30%, transparent);
    }
    .dismiss {
      text-align: center;
    }
    .dismiss-link {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-faint, var(--hu-text-muted));
      background: none;
      border: none;
      cursor: pointer;
      text-decoration: none;
      font-family: var(--hu-font);
      padding: var(--hu-space-sm);
      transition: color var(--hu-duration-normal) var(--hu-ease-out);
      letter-spacing: 0.01em;
    }
    .dismiss-link:hover {
      color: var(--hu-text);
      text-decoration: underline;
      text-underline-offset: 3px;
    }
    .dismiss-link:focus-visible {
      outline: var(--hu-focus-ring-width, 2px) solid var(--hu-focus-ring);
      outline-offset: 2px;
      border-radius: var(--hu-radius-sm, 4px);
    }
    .celebration {
      text-align: center;
      padding: var(--hu-space-2xl, 2rem);
    }
    .celebration .check-icon {
      width: 3.5rem;
      height: 3.5rem;
      color: var(--hu-success);
      margin: 0 auto var(--hu-space-lg, 1.5rem);
      animation: hu-icon-pop var(--hu-duration-slow) var(--hu-ease-out);
    }
    .celebration .check-icon svg {
      width: 100%;
      height: 100%;
    }
    @keyframes hu-icon-pop {
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
      font-size: var(--hu-text-xl, 1.25rem);
      font-weight: var(--hu-weight-bold, 700);
      letter-spacing: -0.02em;
      margin: 0 0 var(--hu-space-xs);
    }
    .celebration p {
      font-size: var(--hu-text-sm);
      color: var(--hu-text-muted);
      margin: 0;
      max-width: 32ch;
      margin-inline: auto;
      line-height: var(--hu-leading-relaxed);
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
      description: "h-uman is connecting to your local gateway. This happens automatically.",
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
      description: "Send your first message and see h-uman in action.",
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
        <hu-card elevated class="celebration">
          <div class="check-icon">${icons.check}</div>
          <h3>You're all set!</h3>
          <p>h-uman is configured and ready. Explore the dashboard to manage your AI assistant.</p>
        </hu-card>
      `;
    }

    const done = this.steps.filter((s) => s.done).length;
    const total = this.steps.length;

    return html`
      <div class="welcome" role="region" aria-label="Welcome and setup guide">
        <div class="greeting">
          <h2>Welcome to h-uman</h2>
          <p>Let's get you set up. ${done}/${total} steps complete.</p>
        </div>
        <div class="steps">
          ${this.steps.map(
            (step) => html`
              <hu-card
                hoverable
                accent
                class="step-card"
                @click=${() => this._navigate(step.action)}
              >
                <div class="step-icon-wrap ${step.done ? "done" : ""}">
                  <div class="step-icon ${step.done ? "done" : ""}">
                    ${step.done ? icons.check : icons[step.icon]}
                  </div>
                </div>
                <div class="step-title">${step.title}</div>
                <div class="step-desc">${step.description}</div>
                ${!step.done
                  ? html`<hu-button variant="secondary" size="sm">Go</hu-button>`
                  : nothing}
                <div class="step-progress ${step.done ? "done" : "pending"}"></div>
              </hu-card>
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
    "hu-welcome": ScWelcome;
  }
}
