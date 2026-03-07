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

export function formatTime(ts: number): string {
  const d = new Date(ts);
  return d.toLocaleTimeString(undefined, {
    hour: "2-digit",
    minute: "2-digit",
  });
}

export function formatTimestampForDivider(ts: number): string {
  const d = new Date(ts);
  const now = new Date();
  const isToday =
    d.getDate() === now.getDate() &&
    d.getMonth() === now.getMonth() &&
    d.getFullYear() === now.getFullYear();
  const time = d.toLocaleTimeString(undefined, {
    hour: "2-digit",
    minute: "2-digit",
  });
  if (isToday) return `Today ${time}`;
  const yesterday = new Date(now);
  yesterday.setDate(yesterday.getDate() - 1);
  const isYesterday =
    d.getDate() === yesterday.getDate() &&
    d.getMonth() === yesterday.getMonth() &&
    d.getFullYear() === yesterday.getFullYear();
  if (isYesterday) return `Yesterday ${time}`;
  return d.toLocaleDateString(undefined, {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  });
}

export const EVENT_NAMES = {
  CHAT: "chat",
  TOOL_CALL: "agent.tool",
  ERROR: "error",
  HEALTH: "health",
} as const;
