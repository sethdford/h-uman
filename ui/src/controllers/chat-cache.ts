import type { ChatItem } from "./chat-controller.js";

const KEY_PREFIX = "sc-chat-";

function storageKey(sessionKey: string): string {
  return `${KEY_PREFIX}${sessionKey}`;
}

function parseCached(raw: string): ChatItem[] {
  const cached = JSON.parse(raw) as unknown;
  if (!Array.isArray(cached) || cached.length === 0) return [];
  return cached
    .map((item: unknown) => {
      const obj = item as Record<string, unknown>;
      if (obj?.type === "message" || obj?.type === "tool_call" || obj?.type === "thinking") {
        return item as ChatItem;
      }
      if (obj?.role && obj?.content) {
        return {
          type: "message",
          role: obj.role as "user" | "assistant",
          content: String(obj.content ?? ""),
        } as ChatItem;
      }
      return null;
    })
    .filter((i): i is ChatItem => i != null);
}

export class ChatCache {
  static save(key: string, items: ChatItem[]): void {
    try {
      sessionStorage.setItem(storageKey(key), JSON.stringify(items));
    } catch {
      /* quota exceeded — ignore */
    }
  }

  static restore(key: string): ChatItem[] {
    try {
      const raw = sessionStorage.getItem(storageKey(key));
      if (!raw) return [];
      const items = parseCached(raw);
      return items.length > 0 ? items : [];
    } catch {
      console.warn("[chat-cache] failed to restore cache:", storageKey(key));
    }
    return [];
  }

  static clear(key: string): void {
    try {
      sessionStorage.removeItem(storageKey(key));
    } catch {
      /* ignore */
    }
  }
}
