import { log } from "./lib/log.js";

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
  #authToken = "";
  #ws: WebSocket | null = null;
  #onBinaryChunk: ((data: ArrayBuffer) => void) | null = null;
  #pending = new Map<
    string,
    {
      resolve: PendingResolve;
      reject: PendingReject;
      timeout: ReturnType<typeof setTimeout>;
    }
  >();
  #pendingReconnect: Array<{
    id: string;
    resolve: PendingResolve;
    reject: PendingReject;
    timeout: ReturnType<typeof setTimeout>;
    raw: string;
  }> = [];
  #pendingReconnectTimer: ReturnType<typeof setTimeout> | null = null;
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

  setAuthToken(token: string): void {
    this.#authToken = token;
  }

  connect(url: string): void {
    if (this.#ws?.readyState === WebSocket.OPEN) {
      return;
    }
    this.#url = url;
    this.#setStatus("connecting");
    try {
      this.#ws = new WebSocket(url);
      this.#ws.binaryType = "arraybuffer";
      this.#ws.onopen = () => {
        this.#reconnectAttempts = 0;
        this.#setStatus("connected");
        this.#replayPendingReconnect();
        this.#sendConnect();
      };
      this.#ws.onclose = () => {
        this.#holdPendingForReconnect();
        this.#setStatus("disconnected");
        this.#scheduleReconnect();
      };
      this.#ws.onerror = () => {
        this.#rejectPending("Connection error");
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
      const connectParams: Record<string, unknown> = {
        client: "human-ui",
        version: "0.3.0",
      };
      if (this.#authToken) connectParams.token = this.#authToken;
      const res = await this.request<{
        type?: string;
        server?: { version?: string };
        features?: ServerFeatures;
        protocol?: number;
      }>("connect", connectParams);
      if (res && typeof res === "object") {
        const payload = res as Record<string, unknown>;
        if (payload.features) this.#features = payload.features as ServerFeatures;
      }
      this.dispatchEvent(new CustomEvent("features", { detail: this.#features }));
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Handshake failed";
      this.dispatchEvent(
        new CustomEvent(GatewayClient.EVENT_GATEWAY, {
          detail: { event: "error", payload: { message: `Connect: ${msg}` } },
        }),
      );
    }
  }

  #setStatus(s: GatewayStatus): void {
    if (this.#status === s) return;
    this.#status = s;
    this.dispatchEvent(new CustomEvent(GatewayClient.EVENT_STATUS, { detail: s }));
  }

  #rejectPending(reason: string): void {
    for (const p of this.#pending.values()) {
      clearTimeout(p.timeout);
      p.reject(new Error(reason));
    }
    this.#pending.clear();
  }

  #holdPendingForReconnect(): void {
    // Move pending requests to reconnect buffer instead of rejecting immediately
    for (const [id, p] of this.#pending.entries()) {
      clearTimeout(p.timeout);
      // We don't have the raw JSON anymore, so store enough to replay
      this.#pendingReconnect.push({
        id,
        resolve: p.resolve,
        reject: p.reject,
        timeout: p.timeout,
        raw: "", // original message already sent; responses keyed by id
      });
    }
    this.#pending.clear();

    // Start a 5-second timer — if no reconnection, reject all
    if (this.#pendingReconnect.length > 0 && !this.#pendingReconnectTimer) {
      this.#pendingReconnectTimer = setTimeout(() => {
        this.#pendingReconnectTimer = null;
        for (const p of this.#pendingReconnect) {
          p.reject(new Error("Connection closed"));
        }
        this.#pendingReconnect = [];
      }, 5000);
    }
  }

  #replayPendingReconnect(): void {
    if (this.#pendingReconnectTimer) {
      clearTimeout(this.#pendingReconnectTimer);
      this.#pendingReconnectTimer = null;
    }
    // Re-register held requests so responses are matched by id
    for (const p of this.#pendingReconnect) {
      const timeout = setTimeout(() => {
        if (this.#pending.delete(p.id)) {
          p.reject(new Error("Request timeout"));
        }
      }, 10000);
      this.#pending.set(p.id, { resolve: p.resolve, reject: p.reject, timeout });
    }
    this.#pendingReconnect = [];
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

  setOnBinaryChunk(handler: ((data: ArrayBuffer) => void) | null): void {
    this.#onBinaryChunk = handler;
  }

  sendBinary(data: ArrayBuffer | ArrayBufferView): void {
    if (this.#ws?.readyState !== WebSocket.OPEN) {
      throw new Error("WebSocket not connected");
    }
    if (data instanceof ArrayBuffer) {
      this.#ws.send(data);
      return;
    }
    // ArrayBufferView (including Uint8Array): copy into a fresh ArrayBuffer
    // so the type matches WebSocket.send (BufferSource) regardless of the
    // underlying buffer type (ArrayBuffer vs SharedArrayBuffer).
    const copy = new Uint8Array(data.byteLength);
    copy.set(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
    this.#ws.send(copy.buffer);
  }

  voiceSessionStart(params?: Record<string, unknown>): Promise<{
    session_id?: string;
    sample_rate?: number;
    input_sample_rate?: number;
    output_sample_rate?: number;
    encoding?: string;
    mode?: string;
  }> {
    return this.request("voice.session.start", params);
  }

  voiceSessionStop(): Promise<unknown> {
    return this.request("voice.session.stop", {});
  }

  voiceSessionInterrupt(): Promise<unknown> {
    return this.request("voice.session.interrupt", {});
  }

  voiceAudioEnd(params?: Record<string, unknown>): Promise<unknown> {
    return this.request("voice.audio.end", params);
  }

  voiceToolResponse(params: { name: string; call_id: string; result: string }): Promise<unknown> {
    return this.request("voice.tool_response", params);
  }

  voiceValidate(params: {
    mode: string;
    apiKey: string;
  }): Promise<{ ok: boolean; mode?: string; error?: string }> {
    return this.request("voice.validate", params) as Promise<{
      ok: boolean;
      mode?: string;
      error?: string;
    }>;
  }

  #onMessage(ev: MessageEvent): void {
    if (ev.data instanceof ArrayBuffer) {
      this.#onBinaryChunk?.(ev.data);
      return;
    }
    if (ev.data instanceof Blob) {
      void ev.data.arrayBuffer().then((ab) => this.#onBinaryChunk?.(ab));
      return;
    }
    if (typeof ev.data !== "string") {
      return;
    }
    let data: Record<string, unknown>;
    try {
      data = JSON.parse(ev.data) as Record<string, unknown>;
    } catch {
      log.warn("[gateway] failed to parse message:", ev.data);
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
    if (this.#pendingReconnectTimer) {
      clearTimeout(this.#pendingReconnectTimer);
      this.#pendingReconnectTimer = null;
    }
    for (const p of this.#pendingReconnect) {
      p.reject(new Error("Disconnected"));
    }
    this.#pendingReconnect = [];
    for (const p of this.#pending.values()) {
      clearTimeout(p.timeout);
      p.reject(new Error("Disconnected"));
    }
    this.#pending.clear();
    this.#onBinaryChunk = null;
    if (this.#ws) {
      this.#ws.close();
      this.#ws = null;
    }
    this.#url = "";
    this.#setStatus("disconnected");
  }
}
