import { LitElement, html, css, nothing } from "lit";
import { customElement, property, state } from "lit/decorators.js";
import { icons } from "../icons.js";

type PresetId =
  | "every-hour"
  | "every-6h"
  | "every-day"
  | "every-weekday"
  | "every-monday"
  | "every-sunday"
  | "1st-month"
  | "every-15min";

interface PresetDef {
  id: PresetId;
  label: string;
  expr: string;
  needsHour: boolean;
}

const PRESETS: PresetDef[] = [
  { id: "every-hour", label: "Every hour", expr: "0 * * * *", needsHour: false },
  { id: "every-6h", label: "Every 6 hours", expr: "0 */6 * * *", needsHour: false },
  { id: "every-15min", label: "Every 15 minutes", expr: "*/15 * * * *", needsHour: false },
  { id: "every-day", label: "Every day at…", expr: "0 8 * * *", needsHour: true },
  { id: "every-weekday", label: "Every weekday at…", expr: "0 8 * * 1-5", needsHour: true },
  { id: "every-monday", label: "Every Monday at…", expr: "0 8 * * 1", needsHour: true },
  { id: "every-sunday", label: "Every week on Sunday at…", expr: "0 8 * * 0", needsHour: true },
  { id: "1st-month", label: "1st of every month at…", expr: "0 8 1 * *", needsHour: true },
];

function hourTo12h(h: number): string {
  if (h === 0) return "12:00 AM";
  if (h === 12) return "12:00 PM";
  return h < 12 ? `${h}:00 AM` : `${h - 12}:00 PM`;
}

function cronWithHour(expr: string, hour: number): string {
  const parts = expr.split(" ");
  if (parts.length >= 2) parts[1] = String(hour);
  return parts.join(" ");
}

function cronToHuman(expr: string): string {
  const parts = expr.trim().split(/\s+/);
  if (parts.length < 5) return expr;

  const [min, hour, dom, month, dow] = parts;

  if (min === "*/15" && hour === "*" && dom === "*" && month === "*" && dow === "*")
    return "Every 15 minutes";
  if (min === "0" && hour === "*" && dom === "*" && month === "*" && dow === "*")
    return "Every hour";
  if (min === "0" && hour === "*/6" && dom === "*" && month === "*" && dow === "*")
    return "Every 6 hours";

  const h = parseInt(hour, 10);
  const hourStr = !isNaN(h) ? hourTo12h(h) : hour;

  if (min === "0" && dom === "*" && month === "*" && dow === "*") return `Every day at ${hourStr}`;
  if (min === "0" && dom === "*" && month === "*" && dow === "1-5")
    return `Every weekday at ${hourStr}`;
  if (min === "0" && dom === "*" && month === "*" && dow === "1")
    return `Every Monday at ${hourStr}`;
  if (min === "0" && dom === "*" && month === "*" && dow === "0")
    return `Every week on Sunday at ${hourStr}`;
  if (min === "0" && dom === "1" && month === "*" && dow === "*")
    return `1st of every month at ${hourStr}`;

  return expr;
}

function isValidCron(expr: string): boolean {
  const parts = expr.trim().split(/\s+/);
  return parts.length === 5 || parts.length === 6;
}

function parseNaturalSchedule(text: string): string | null {
  const t = text.toLowerCase().trim();
  if (!t) return null;

  const minMatch = t.match(/every\s+(\d+)\s+min/);
  if (minMatch) return `*/${minMatch[1]} * * * *`;

  if (/every\s+hour/.test(t)) return "0 * * * *";

  const dailyMatch = t.match(/every\s+day\s+at\s+(\d{1,2})\s*(am|pm)?/i);
  if (dailyMatch) {
    let h = parseInt(dailyMatch[1], 10);
    if (dailyMatch[2]?.toLowerCase() === "pm" && h < 12) h += 12;
    if (dailyMatch[2]?.toLowerCase() === "am" && h === 12) h = 0;
    return `0 ${h} * * *`;
  }

  if (/every\s+morning/.test(t)) return "0 8 * * *";
  if (/every\s+evening/.test(t)) return "0 18 * * *";
  if (/every\s+night/.test(t)) return "0 22 * * *";

  const wdMatch = t.match(/every\s+weekday\s+at\s+(\d{1,2})\s*(am|pm)?/i);
  if (wdMatch) {
    let h = parseInt(wdMatch[1], 10);
    if (wdMatch[2]?.toLowerCase() === "pm" && h < 12) h += 12;
    return `0 ${h} * * 1-5`;
  }

  if (/every\s+weekday/.test(t)) return "0 9 * * 1-5";

  const days: Record<string, string> = {
    sunday: "0",
    monday: "1",
    tuesday: "2",
    wednesday: "3",
    thursday: "4",
    friday: "5",
    saturday: "6",
  };
  for (const [day, num] of Object.entries(days)) {
    const re = new RegExp(`every\\s+${day}\\s+at\\s+(\\d{1,2})\\s*(am|pm)?`, "i");
    const m = t.match(re);
    if (m) {
      let h = parseInt(m[1], 10);
      if (m[2]?.toLowerCase() === "pm" && h < 12) h += 12;
      return `0 ${h} * * ${num}`;
    }
    if (new RegExp(`every\\s+${day}`, "i").test(t)) {
      return `0 9 * * ${num}`;
    }
  }

  if (/^hourly$/.test(t)) return "0 * * * *";
  if (/^daily$/.test(t)) return "0 9 * * *";
  if (/^weekly$/.test(t)) return "0 9 * * 1";
  if (/^monthly$/.test(t)) return "0 9 1 * *";

  return null;
}

function presetIdForExpr(expr: string): PresetId | null {
  const parts = expr.trim().split(/\s+/);
  if (parts.length < 5) return null;

  const [min, hour, dom, month, dow] = parts;

  if (min === "*/15" && hour === "*" && dom === "*" && month === "*" && dow === "*")
    return "every-15min";
  if (min === "0" && hour === "*" && dom === "*" && month === "*" && dow === "*")
    return "every-hour";
  if (min === "0" && hour === "*/6" && dom === "*" && month === "*" && dow === "*")
    return "every-6h";
  if (min === "0" && dom === "*" && month === "*" && dow === "*") return "every-day";
  if (min === "0" && dom === "*" && month === "*" && dow === "1-5") return "every-weekday";
  if (min === "0" && dom === "*" && month === "*" && dow === "1") return "every-monday";
  if (min === "0" && dom === "*" && month === "*" && dow === "0") return "every-sunday";
  if (min === "0" && dom === "1" && month === "*" && dow === "*") return "1st-month";

  return null;
}

@customElement("hu-schedule-builder")
export class ScScheduleBuilder extends LitElement {
  static override styles = css`
    :host {
      display: block;
      font-family: var(--hu-font);
    }

    .wrapper {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-md);
    }

    .preset-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(10rem, 1fr));
      gap: var(--hu-space-sm);
    }

    .preset-card {
      display: flex;
      flex-direction: column;
      align-items: stretch;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-md);
      background: var(--hu-bg-surface);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      cursor: pointer;
      text-align: left;
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out);
    }

    .preset-card:hover {
      border-color: var(--hu-accent);
      background: var(--hu-accent-subtle);
    }

    .preset-card:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .preset-card.selected {
      border-color: var(--hu-accent);
      background: var(--hu-accent-subtle);
      box-shadow: 0 0 0 1px var(--hu-accent);
    }

    .preset-label {
      font-size: var(--hu-text-sm);
      font-weight: var(--hu-weight-medium);
      color: var(--hu-text);
    }

    .preset-hour-row {
      display: flex;
      align-items: center;
      gap: var(--hu-space-xs);
    }

    .preset-hour-row label {
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
      flex-shrink: 0;
    }

    .hour-select {
      flex: 1;
      min-width: 0;
      padding: var(--hu-space-xs) var(--hu-space-sm);
      font-size: var(--hu-text-sm);
      font-family: var(--hu-font);
      background: var(--hu-bg);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius-sm);
      color: var(--hu-text);
      cursor: pointer;
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        background var(--hu-duration-fast) var(--hu-ease-out);
    }

    .hour-select:hover:not(:disabled) {
      border-color: var(--hu-text-faint);
    }

    .hour-select:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .custom-row {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-sm);
    }

    .custom-input {
      width: 100%;
      box-sizing: border-box;
      padding: var(--hu-space-sm) var(--hu-space-md);
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-sm);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out);
    }

    .custom-input::placeholder {
      color: var(--hu-text-muted);
    }

    .custom-input:focus {
      outline: none;
      border-color: var(--hu-accent);
      box-shadow: 0 0 0 0.1875rem var(--hu-accent-subtle);
    }

    .custom-input:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .custom-input.invalid {
      border-color: var(--hu-error);
    }

    .custom-error {
      font-size: var(--hu-text-sm);
      color: var(--hu-error);
    }

    .nl-input {
      width: 100%;
      box-sizing: border-box;
      padding: var(--hu-space-sm) var(--hu-space-md);
      font-family: var(--hu-font);
      font-size: var(--hu-text-sm);
      background: var(--hu-bg-elevated);
      border: 1px solid var(--hu-border);
      border-radius: var(--hu-radius);
      color: var(--hu-text);
      transition:
        border-color var(--hu-duration-fast) var(--hu-ease-out),
        box-shadow var(--hu-duration-fast) var(--hu-ease-out);
    }

    .nl-input::placeholder {
      color: var(--hu-text-muted);
    }

    .nl-input:focus {
      outline: none;
      border-color: var(--hu-accent);
      box-shadow: 0 0 0 0.1875rem var(--hu-accent-subtle);
    }

    .nl-hint {
      font-size: var(--hu-text-xs);
      margin-top: var(--hu-space-xs);
    }

    .nl-hint.success {
      color: var(--hu-accent);
    }

    .nl-hint.unknown {
      color: var(--hu-text-muted);
    }

    .nl-row {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
    }

    .mode-link {
      display: inline-flex;
      align-items: center;
      gap: var(--hu-space-xs);
      padding: var(--hu-space-xs) 0;
      font-size: var(--hu-text-sm);
      color: var(--hu-accent);
      background: none;
      border: none;
      cursor: pointer;
      font-family: var(--hu-font);
      transition: color var(--hu-duration-fast) var(--hu-ease-out);
    }

    .mode-link:hover {
      color: var(--hu-accent-hover);
      text-decoration: underline;
    }

    .mode-link:focus-visible {
      outline: 2px solid var(--hu-accent);
      outline-offset: 2px;
    }

    .mode-link svg {
      width: 0.875rem;
      height: 0.875rem;
    }

    .footer {
      display: flex;
      flex-direction: column;
      gap: var(--hu-space-xs);
      padding-top: var(--hu-space-sm);
      border-top: 1px solid var(--hu-border);
    }

    .expr-display {
      font-family: var(--hu-font-mono);
      font-size: var(--hu-text-xs);
      color: var(--hu-text-muted);
    }

    .human-display {
      font-size: var(--hu-text-sm);
      color: var(--hu-text);
    }

    @media (prefers-reduced-motion: reduce) {
      .preset-card,
      .hour-select,
      .custom-input,
      .mode-link {
        transition: none;
      }
    }
  `;

  @property({ type: String }) value = "0 8 * * *";

  @property({ type: String }) mode: "preset" | "custom" = "preset";

  @state() private _selectedPreset: PresetId | null = "every-day";

  @state() private _hour = 8;

  @state() private _customInput = "";

  @state() private _customError = "";

  @state() private _nlInput = "";

  @state() private _nlResult: string | null = null;

  private _nlDebounceTimer: ReturnType<typeof setTimeout> | null = null;

  private _emitChange(val: string): void {
    this.value = val;
    this.dispatchEvent(
      new CustomEvent("hu-schedule-change", {
        bubbles: true,
        composed: true,
        detail: { value: val },
      }),
    );
  }

  private _selectPreset(preset: PresetDef): void {
    this._selectedPreset = preset.id;
    const hour = preset.needsHour ? this._hour : 8;
    if (preset.needsHour) {
      const expr = cronWithHour(preset.expr, hour);
      this._emitChange(expr);
    } else {
      this._emitChange(preset.expr);
    }
  }

  private _onHourChange(e: Event): void {
    const select = e.target as HTMLSelectElement;
    const h = parseInt(select.value, 10);
    if (isNaN(h) || h < 0 || h > 23) return;
    this._hour = h;
    const preset = PRESETS.find((p) => p.id === this._selectedPreset);
    if (preset?.needsHour) {
      this._emitChange(cronWithHour(preset.expr, h));
    }
  }

  private _switchToCustom(): void {
    this.mode = "custom";
    this._customInput = this.value;
    this._customError = "";
  }

  private _switchToPreset(): void {
    this.mode = "preset";
    this._customError = "";
    const pid = presetIdForExpr(this.value);
    this._selectedPreset = pid;
    const parts = this.value.trim().split(/\s+/);
    const h = parseInt(parts[1], 10);
    if (!isNaN(h) && h >= 0 && h <= 23) this._hour = h;
  }

  private _onNlInput(e: Event): void {
    const input = e.target as HTMLInputElement;
    this._nlInput = input.value;
    if (this._nlDebounceTimer) clearTimeout(this._nlDebounceTimer);
    this._nlDebounceTimer = setTimeout(() => {
      this._nlDebounceTimer = null;
      const t = this._nlInput.trim();
      if (!t) {
        this._nlResult = null;
        return;
      }
      const parsed = parseNaturalSchedule(t);
      if (parsed) {
        this._nlResult = parsed;
        this._emitChange(parsed);
        const pid = presetIdForExpr(parsed);
        this._selectedPreset = pid;
        const parts = parsed.trim().split(/\s+/);
        const h = parseInt(parts[1], 10);
        if (!isNaN(h) && h >= 0 && h <= 23) this._hour = h;
      } else {
        this._nlResult = null;
      }
    }, 300);
  }

  private _onCustomInput(e: Event): void {
    const input = e.target as HTMLInputElement;
    const val = input.value.trim();
    this._customInput = input.value;
    if (!val) {
      this._customError = "";
      return;
    }
    if (!isValidCron(val)) {
      this._customError = "Must be 5 or 6 space-separated fields (e.g. 0 8 * * *)";
      return;
    }
    this._customError = "";
    this._emitChange(val);
  }

  protected override willUpdate(changed: Map<string, unknown>): void {
    if (changed.has("value") && this.mode === "preset") {
      this._selectedPreset = presetIdForExpr(this.value);
      const parts = this.value.trim().split(/\s+/);
      const h = parseInt(parts[1], 10);
      if (!isNaN(h) && h >= 0 && h <= 23) this._hour = h;
    }
    if (changed.has("mode") && this.mode === "custom") {
      this._customInput = this.value;
    }
  }

  override render() {
    const hourOptions = Array.from({ length: 24 }, (_, i) => ({
      value: i,
      label: hourTo12h(i),
    }));

    return html`
      <div class="wrapper" role="group" aria-label="Schedule builder">
        ${this.mode === "preset"
          ? html`
              <div class="nl-row">
                <input
                  type="text"
                  class="nl-input"
                  .value=${this._nlInput}
                  placeholder="e.g. every weekday at 9am"
                  aria-label="Describe your schedule"
                  @input=${this._onNlInput}
                />
                ${this._nlInput.trim()
                  ? this._nlResult
                    ? html`<span class="nl-hint success" role="status"
                        >${cronToHuman(this._nlResult)}</span
                      >`
                    : html`<span class="nl-hint unknown" role="status">Not recognized</span>`
                  : nothing}
              </div>
              <div class="preset-grid" role="list">
                ${PRESETS.map(
                  (preset) => html`
                    <div
                      class="preset-card ${this._selectedPreset === preset.id ? "selected" : ""}"
                      role="listitem"
                      tabindex="0"
                      @click=${() => this._selectPreset(preset)}
                      @keydown=${(e: KeyboardEvent) => {
                        if (e.key === "Enter" || e.key === " ") {
                          e.preventDefault();
                          this._selectPreset(preset);
                        }
                      }}
                    >
                      <span class="preset-label">${preset.label}</span>
                      ${preset.needsHour && this._selectedPreset === preset.id
                        ? html`
                            <div class="preset-hour-row">
                              <label for="hour-${preset.id}">Time</label>
                              <select
                                id="hour-${preset.id}"
                                class="hour-select"
                                .value=${String(this._hour)}
                                @change=${this._onHourChange}
                                @click=${(e: Event) => e.stopPropagation()}
                                aria-label="Hour (0-23)"
                              >
                                ${hourOptions.map(
                                  (opt) => html`<option value=${opt.value}>${opt.label}</option>`,
                                )}
                              </select>
                            </div>
                          `
                        : nothing}
                    </div>
                  `,
                )}
              </div>
              <button
                type="button"
                class="mode-link"
                @click=${this._switchToCustom}
                aria-label="Switch to custom cron expression"
              >
                ${icons["pencil-simple"]} Custom
              </button>
            `
          : html`
              <div class="custom-row">
                <input
                  type="text"
                  class="custom-input ${this._customError ? "invalid" : ""}"
                  .value=${this._customInput}
                  placeholder="0 8 * * *"
                  aria-label="Cron expression"
                  aria-invalid=${this._customError ? "true" : "false"}
                  aria-describedby=${this._customError ? "custom-error" : undefined}
                  @input=${this._onCustomInput}
                />
                ${this._customError
                  ? html`<span class="custom-error" id="custom-error" role="alert"
                      >${this._customError}</span
                    >`
                  : nothing}
                <button
                  type="button"
                  class="mode-link"
                  @click=${this._switchToPreset}
                  aria-label="Back to presets"
                >
                  ${icons["arrow-left"]} Back to presets
                </button>
              </div>
            `}

        <div class="footer">
          <code class="expr-display">${this.value || "—"}</code>
          <span class="human-display">${cronToHuman(this.value) || "—"}</span>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    "hu-schedule-builder": ScScheduleBuilder;
  }
}
