import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";
import "./hu-card.js";
import "./hu-button.js";

const ONBOARDED_KEY = "hu-onboarded";

@customElement("hu-welcome-card")
export class ScWelcomeCard extends LitElement {
  static override styles = css`
    :host {
      display: block;
    }

    .card {
      animation: hu-welcome-card-enter var(--hu-duration-moderate) var(--hu-spring-out) both;
    }

    @keyframes hu-welcome-card-enter {
      from {
        opacity: 0;
        transform: translateY(var(--hu-space-lg)) scale(0.98);
      }
      to {
        opacity: 1;
        transform: translateY(0) scale(1);
      }
    }

    @media (prefers-reduced-motion: reduce) {
      .card {
        animation: none;
      }
    }

    .hero {
      display: flex;
      flex-direction: column;
      align-items: center;
      text-align: center;
      margin-bottom: var(--hu-space-xl);
    }

    .hero-icon {
      width: 3rem;
      height: 3rem;
      margin-bottom: var(--hu-space-md);
      color: var(--hu-accent);
    }

    .hero-icon svg {
      width: 100%;
      height: 100%;
    }

    .hero h2 {
      font-size: clamp(1.5rem, 2.5vw, 1.75rem);
      font-weight: var(--hu-weight-bold);
      letter-spacing: -0.03em;
      color: var(--hu-text);
      margin: 0 0 var(--hu-space-xs);
    }

    .hero p {
      font-size: var(--hu-text-base);
      color: var(--hu-text-muted);
      margin: 0;
      line-height: var(--hu-leading-relaxed);
      max-width: 36ch;
    }

    .features {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: var(--hu-space-md);
      margin-bottom: var(--hu-space-xl);
    }

    .feature {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: var(--hu-space-xs);
      text-align: center;
    }

    .feature-icon {
      width: 2rem;
      height: 2rem;
      color: var(--hu-accent);
    }

    .feature-icon svg {
      width: 100%;
      height: 100%;
    }

    .feature span {
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
    }

    .cta {
      display: flex;
      justify-content: center;
    }

    @media (max-width: 480px) /* --hu-breakpoint-sm */ {
      .features {
        grid-template-columns: 1fr;
      }
    }
  `;

  @property({ type: Boolean }) visible = true;
  @property({ type: String }) userName = "";

  @state() private _dismissed = false;

  private get _show(): boolean {
    return this.visible && !this._dismissed && localStorage.getItem(ONBOARDED_KEY) !== "true";
  }

  private _onGetStarted(): void {
    this._dismissed = true;
    localStorage.setItem(ONBOARDED_KEY, "true");
    this.dispatchEvent(new CustomEvent("dismiss", { bubbles: true, composed: true }));
  }

  override render() {
    if (!this._show) return nothing;

    const greeting = this.userName ? `Welcome to Human, ${this.userName}` : "Welcome to Human";

    return html`
      <hu-card elevated accent glass class="card">
        <div class="hero">
          <div class="hero-icon" aria-hidden="true">${icons.zap}</div>
          <h2>${greeting}</h2>
          <p>Your autonomous AI assistant runtime</p>
        </div>
        <div class="features">
          <div class="feature">
            <div class="feature-icon">${icons["message-square"]}</div>
            <span>Chat with AI</span>
          </div>
          <div class="feature">
            <div class="feature-icon">${icons.wrench}</div>
            <span>Run tools</span>
          </div>
          <div class="feature">
            <div class="feature-icon">${icons.zap}</div>
            <span>Manage agents</span>
          </div>
        </div>
        <div class="cta">
          <hu-button variant="primary" @click=${this._onGetStarted}>Get Started</hu-button>
        </div>
      </hu-card>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-welcome-card": ScWelcomeCard;
  }
}
