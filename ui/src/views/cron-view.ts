/**
 * Legacy alias: sc-cron-view redirects to sc-automations-view.
 * Kept for backward compatibility; new code should use automations-view.
 */
import { ScAutomationsView } from "./automations-view.js";

customElements.define("sc-cron-view", ScAutomationsView);
