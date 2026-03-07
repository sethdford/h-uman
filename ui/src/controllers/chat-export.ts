import type { ChatItem } from "./chat-controller.js";

export function exportAsMarkdown(items: ChatItem[]): string {
  const lines: string[] = [];
  for (const item of items) {
    if (item.type === "message") {
      const role = item.role === "user" ? "**You**" : "**Assistant**";
      lines.push(`${role}: ${item.content}\n`);
    } else if (item.type === "tool_call") {
      lines.push(`> Tool: ${item.name} \u2192 ${item.result ?? "running"}\n`);
    }
  }
  return lines.join("\n");
}

export function exportAsJson(items: ChatItem[]): string {
  return JSON.stringify(items, null, 2);
}
