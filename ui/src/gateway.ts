export type GatewayStatus = "disconnected" | "connecting" | "connected";

export interface GatewayRequest {
  id: string;
  type: "req";
  method: string;
  params?: Record<string, unknown>;
}

export interface GatewayResponse {
  id: string;
  type: "res";
  ok?: boolean;
  result?: unknown;
  payload?: unknown;
  error?: { code: number; message: string };
}

export interface ServerFeatures {
  methods?: string[];
  sessions?: boolean;
  cron?: boolean;
  skills?: boolean;
  cost_tracking?: boolean;
}

type PendingResolve = (value: GatewayResponse) => void;
type PendingReject = (reason: Error) => void;

export class GatewayClient extends EventTarget {
  #url = "";
  #ws: WebSocket | null = null;
  #pending = new Map<
    string,
    {
      resolve: PendingResolve;
      reject: PendingReject;
      timeout: ReturnType<typeof setTimeout>;
    }
  >();
  #status: GatewayStatus = "disconnected";
  #reconnectAttempts = 0;
  #reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  #nextId = 1;
  #features: ServerFeatures = {};

  static readonly EVENT_STATUS = "status";
  static readonly EVENT_MESSAGE = "message";
  static readonly EVENT_GATEWAY = "gateway";

  get status(): GatewayStatus {
    return this.#status;
  }

  get features(): ServerFeatures {
    return this.#features;
  }

  connect(url: string): void {
    if (this.#ws?.readyState === WebSocket.OPEN) {
      return;
    }
    this.#url = url;
    this.#setStatus("connecting");
    try {
      this.#ws = new WebSocket(url);
      this.#ws.onopen = () => {
        this.#reconnectAttempts = 0;
        this.#setStatus("connected");
        this.#sendConnect();
      };
      this.#ws.onclose = () => {
        this.#setStatus("disconnected");
        this.#scheduleReconnect();
      };
      this.#ws.onerror = () => {
        this.#setStatus("disconnected");
        this.#scheduleReconnect();
      };
      this.#ws.onmessage = (ev) => this.#onMessage(ev);
    } catch {
      this.#setStatus("disconnected");
      this.#scheduleReconnect();
    }
  }

  async #sendConnect(): Promise<void> {
    try {
      const res = await this.request<{
        type?: string;
        server?: { version?: string };
        features?: ServerFeatures;
        protocol?: number;
      }>("connect", { client: "human-ui", version: "0.3.0" });
      if (res && typeof res === "object") {
        const payload = res as Record<string, unknown>;
        if (payload.features) this.#features = payload.features as ServerFeatures;
      }
      this.dispatchEvent(new CustomEvent("features", { detail: this.#features }));
    } catch {
      /* connect handshake optional — gateway may not require it */
    }
  }

  #setStatus(s: GatewayStatus): void {
    if (this.#status === s) return;
    this.#status = s;
    this.dispatchEvent(new CustomEvent(GatewayClient.EVENT_STATUS, { detail: s }));
  }

  #scheduleReconnect(): void {
    if (this.#reconnectTimer) return;
    this.#reconnectAttempts++;
    const delay = Math.min(1000 * 2 ** this.#reconnectAttempts, 30000);
    this.#reconnectTimer = setTimeout(() => {
      this.#reconnectTimer = null;
      if (this.#url && this.#status !== "connected") {
        this.connect(this.#url);
      }
    }, delay);
  }

  #onMessage(ev: MessageEvent): void {
    let data: Record<string, unknown>;
    try {
      data = JSON.parse(ev.data as string) as Record<string, unknown>;
    } catch {
      console.warn("[gateway] failed to parse message:", ev.data);
      return;
    }
    const type = data.type as string;

    if (type === "event") {
      const eventName = data.event as string;
      const payload = data.payload ?? {};
      this.dispatchEvent(
        new CustomEvent(GatewayClient.EVENT_GATEWAY, {
          detail: { event: eventName, payload },
        }),
      );
      return;
    }

    if (type === "hello-ok") {
      if (data.features) this.#features = data.features as ServerFeatures;
      this.dispatchEvent(new CustomEvent("features", { detail: this.#features }));
    }

    const id = data.id as string | undefined;
    if (id) {
      const pending = this.#pending.get(id);
      if (pending) {
        clearTimeout(pending.timeout);
        this.#pending.delete(id);
        const err = data.error as { message?: string } | undefined;
        const ok = data.ok as boolean | undefined;
        if (err) {
          pending.reject(new Error(err.message ?? "Unknown error"));
        } else if (ok === false) {
          const p = data.payload as { error?: string } | undefined;
          pending.reject(new Error(p?.error ?? "Request failed"));
        } else {
          const result = (data.payload ?? data.result) as unknown;
          pending.resolve({ ...data, result } as GatewayResponse);
        }
      }
    }
    this.dispatchEvent(new CustomEvent(GatewayClient.EVENT_MESSAGE, { detail: data }));
  }

  request<T = unknown>(
    method: string,
    params?: Record<string, unknown>,
    timeoutMs = 10000,
  ): Promise<T> {
    return new Promise((resolve, reject) => {
      if (this.#ws?.readyState !== WebSocket.OPEN) {
        reject(new Error("WebSocket not connected"));
        return;
      }
      const id = `req-${this.#nextId++}-${Date.now()}`;
      const req: GatewayRequest = { id, type: "req", method, params };
      const timeout = setTimeout(() => {
        if (this.#pending.delete(id)) {
          reject(new Error("Request timeout"));
        }
      }, timeoutMs);
      this.#pending.set(id, {
        resolve: (res) => resolve(((res as { result?: T }).result as T) ?? (res as T)),
        reject,
        timeout,
      });
      this.#ws.send(JSON.stringify(req));
    });
  }

  abort(): Promise<{ aborted?: boolean }> {
    return this.request("chat.abort", {});
  }

  disconnect(): void {
    if (this.#reconnectTimer) {
      clearTimeout(this.#reconnectTimer);
      this.#reconnectTimer = null;
    }
    for (const p of this.#pending.values()) {
      clearTimeout(p.timeout);
      p.reject(new Error("Disconnected"));
    }
    this.#pending.clear();
    if (this.#ws) {
      this.#ws.close();
      this.#ws = null;
    }
    this.#url = "";
    this.#setStatus("disconnected");
  }
}
