export function formatDate(v: number | string | undefined | null): string {
  if (v == null) return "—";
  try {
    const ts = typeof v === "number" ? (v < 1e12 ? v * 1000 : v) : v;
    return new Intl.DateTimeFormat(undefined, {
      dateStyle: "short",
      timeStyle: "short",
    }).format(new Date(ts));
  } catch {
    return String(v);
  }
}

export function formatRelative(v: number | string | undefined | null): string {
  if (v == null) return "—";
  try {
    const ts = typeof v === "number" ? (v < 1e12 ? v * 1000 : v) : Date.parse(String(v));
    const diff = Date.now() - ts;
    if (diff < 60000) return "just now";
    if (diff < 3600000) return `${Math.floor(diff / 60000)}m ago`;
    if (diff < 86400000) return `${Math.floor(diff / 3600000)}h ago`;
    return `${Math.floor(diff / 86400000)}d ago`;
  } catch {
    return "—";
  }
}

export const SESSION_KEY_DEFAULT = "default";
export const SESSION_KEY_VOICE = "voice";

export const EVENT_NAMES = {
  CHAT: "chat",
  TOOL_CALL: "agent.tool",
  ERROR: "error",
  HEALTH: "health",
} as const;
