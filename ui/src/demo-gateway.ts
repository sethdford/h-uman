import type { GatewayStatus, ServerFeatures } from "./gateway.js";

const DEMO_CHANNELS = [
  { key: "telegram", label: "Telegram", configured: true, healthy: true, status: "Connected" },
  { key: "discord", label: "Discord", configured: true, healthy: true, status: "Connected" },
  { key: "slack", label: "Slack", configured: true, healthy: true, status: "Connected" },
  { key: "signal", label: "Signal", configured: false, status: "Not configured" },
  { key: "imessage", label: "iMessage", configured: true, healthy: true, status: "Connected" },
  {
    key: "email",
    label: "Email",
    configured: true,
    healthy: false,
    error: "SMTP timeout",
    status: "Error",
  },
  { key: "matrix", label: "Matrix", configured: false, status: "Not configured" },
  { key: "whatsapp", label: "WhatsApp", configured: true, healthy: true, status: "Connected" },
];

function makeSessions() {
  return [
    {
      key: "sess-a1",
      label: "Project Planning",
      title: "Project Planning",
      created_at: Date.now() - 3600000,
      last_active: Date.now() - 120000,
      updated_at: Date.now() - 120000,
      turn_count: 24,
      messages_count: 24,
      last_message: "Let me review the sprint goals and create a summary.",
      status: "active",
    },
    {
      key: "sess-b2",
      label: "Code Review",
      title: "Code Review",
      created_at: Date.now() - 7200000,
      last_active: Date.now() - 600000,
      updated_at: Date.now() - 600000,
      turn_count: 12,
      messages_count: 12,
      last_message: "The PR looks good. I've added inline suggestions.",
      status: "active",
    },
    {
      key: "sess-c3",
      label: "Bug Investigation",
      title: "Bug Investigation",
      created_at: Date.now() - 86400000,
      last_active: Date.now() - 3600000,
      updated_at: Date.now() - 3600000,
      turn_count: 8,
      messages_count: 8,
      last_message: "Found the issue — missing free() in error path.",
      status: "active",
    },
    {
      key: "sess-d4",
      label: "Architecture Discussion",
      title: "Architecture Discussion",
      created_at: Date.now() - 172800000,
      last_active: Date.now() - 86400000,
      updated_at: Date.now() - 86400000,
      turn_count: 31,
      messages_count: 31,
      last_message: "The vtable pattern fits well for extensibility.",
      status: "archived",
    },
    {
      key: "sess-e5",
      label: "Quick Question",
      title: "Quick Question",
      created_at: Date.now() - 259200000,
      last_active: Date.now() - 172800000,
      updated_at: Date.now() - 172800000,
      turn_count: 3,
      messages_count: 3,
      last_message: "Yes, use sc_arena_alloc for that.",
      status: "archived",
    },
  ];
}

const DEMO_EVENTS = [
  {
    type: "message",
    channel: "Telegram",
    user: "Alice",
    preview: "Can you review the PR?",
    time: Date.now() - 30000,
  },
  { type: "tool_exec", tool: "shell", command: "git status", time: Date.now() - 60000 },
  {
    type: "message",
    channel: "Discord",
    user: "Bob",
    preview: "Deploy looks good",
    time: Date.now() - 120000,
  },
  { type: "session_start", session: "Code Review", time: Date.now() - 180000 },
  {
    type: "message",
    channel: "Slack",
    user: "Charlie",
    preview: "Tests passing on CI",
    time: Date.now() - 240000,
  },
  {
    type: "tool_exec",
    tool: "web_search",
    command: "latest Rust async patterns",
    time: Date.now() - 300000,
  },
];

function makeConfig() {
  return {
    provider: "openrouter",
    default_provider: "openrouter",
    model: "claude-sonnet-4-20250514",
    default_model: "claude-sonnet-4-20250514",
    channels: { telegram: { enabled: true }, discord: { enabled: true } },
    security: {
      autonomy_level: 1,
      sandbox: "auto",
      sandbox_config: {
        enabled: true,
        backend: "landlock",
        net_proxy: {
          enabled: true,
          deny_all: false,
          allowed_domains: ["api.openai.com", "api.anthropic.com", "openrouter.ai"],
        },
      },
    },
    gateway: { require_pairing: true },
    memory: { backend: "sqlite", auto_save: true },
    agent: { persona: "default", max_turns: 50 },
  };
}

function makeCronJobs() {
  return [
    {
      id: 1,
      name: "Daily Summary",
      type: "agent",
      expression: "0 9 * * *",
      schedule: "0 9 * * *",
      prompt: "Summarize overnight emails",
      command: "",
      enabled: true,
      paused: false,
      next_run: Math.floor(Date.now() / 1000) + 86400,
      last_run: Math.floor((Date.now() - 3600000) / 1000),
      created_at: Math.floor((Date.now() - 604800000) / 1000),
    },
    {
      id: 2,
      name: "Health Check",
      type: "shell",
      expression: "*/30 * * * *",
      schedule: "*/30 * * * *",
      prompt: "",
      command: "git pull && npm test",
      enabled: true,
      paused: false,
      next_run: Math.floor(Date.now() / 1000) + 1800,
      last_run: Math.floor((Date.now() - 1800000) / 1000),
      created_at: Math.floor((Date.now() - 259200000) / 1000),
    },
    {
      id: 3,
      name: "Daily Standup",
      type: "agent",
      expression: "0 18 * * 1-5",
      schedule: "0 18 * * 1-5",
      prompt: "Generate daily standup report",
      command: "",
      enabled: false,
      paused: true,
      next_run: 0,
      last_run: 0,
      created_at: Math.floor((Date.now() - 1209600000) / 1000),
    },
  ];
}

function makeSkills() {
  return [
    {
      name: "web-research",
      description: "Deep web research with source citations and fact-checking",
      enabled: true,
      parameters: '{"query": "string", "depth": "number"}',
      tags: "research, web",
    },
    {
      name: "code-review",
      description: "Automated code review with inline suggestions and severity ratings",
      enabled: true,
      parameters: '{"files": "string[]", "strictness": "string"}',
      tags: "development, review",
    },
    {
      name: "data-analysis",
      description: "Analyze CSV, JSON, and SQL datasets with visualizations",
      enabled: true,
      parameters: '{"source": "string"}',
    },
    {
      name: "email-digest",
      description: "Summarize and prioritize inbox messages across providers",
      enabled: false,
    },
    {
      name: "image-gen",
      description: "Generate images via DALL-E, Stable Diffusion, or Flux",
      enabled: false,
    },
    {
      name: "git-assistant",
      description: "Automated git operations: branch, commit, rebase, and PR creation",
      enabled: true,
      parameters: '{"repo": "string"}',
    },
  ];
}

const DEMO_REGISTRY = [
  {
    name: "code-review",
    description: "Automated code review with inline suggestions",
    version: "1.2.0",
    author: "seaclaw",
    url: "https://github.com/seaclaw/skill-registry/tree/main/skills/code-review",
    tags: "development, review",
  },
  {
    name: "email-digest",
    description: "Daily email digest and inbox summarization",
    version: "1.0.0",
    author: "seaclaw",
    url: "https://github.com/seaclaw/skill-registry/tree/main/skills/email-digest",
    tags: "email, productivity",
  },
  {
    name: "web-research",
    description: "Deep web research with source citations",
    version: "2.1.0",
    author: "seaclaw",
    url: "https://github.com/seaclaw/skill-registry/tree/main/skills/web-research",
    tags: "research, web",
  },
  {
    name: "calendar-sync",
    description: "Sync and manage calendar events across providers",
    version: "1.0.0",
    author: "community",
    url: "https://github.com/seaclaw/skill-registry/tree/main/skills/calendar-sync",
    tags: "calendar, productivity",
  },
  {
    name: "slack-bridge",
    description: "Bridge conversations between Slack and other channels",
    version: "0.9.0",
    author: "community",
    url: "https://github.com/seaclaw/skill-registry/tree/main/skills/slack-bridge",
    tags: "communication, slack",
  },
  {
    name: "test-runner",
    description: "Run and report test suites across languages and frameworks",
    version: "1.1.0",
    author: "seaclaw",
    url: "https://github.com/seaclaw/skill-registry/tree/main/skills/test-runner",
    tags: "development, testing",
  },
  {
    name: "deploy-helper",
    description: "Automated deployment to cloud providers with rollback support",
    version: "0.8.0",
    author: "community",
    url: "https://github.com/seaclaw/skill-registry/tree/main/skills/deploy-helper",
    tags: "devops, deployment",
  },
  {
    name: "doc-writer",
    description: "Generate and maintain API documentation from source code",
    version: "1.3.0",
    author: "seaclaw",
    url: "https://github.com/seaclaw/skill-registry/tree/main/skills/doc-writer",
    tags: "documentation, development",
  },
];

const DEMO_NODES = [
  {
    id: "local",
    type: "gateway",
    status: "online",
    hostname: "studio.local",
    version: "0.3.1",
    uptime_secs: 172800,
    ws_connections: 3,
    cpu_percent: 12,
    memory_mb: 5.7,
  },
  {
    id: "remote-prod",
    type: "gateway",
    status: "online",
    hostname: "prod-us-east.example.com",
    version: "0.3.1",
    uptime_secs: 604800,
    ws_connections: 18,
    cpu_percent: 34,
    memory_mb: 14.2,
  },
  {
    id: "rpi-sensor",
    type: "peripheral",
    status: "degraded",
    hostname: "raspberrypi.local",
    version: "0.3.0",
    uptime_secs: 3600,
    ws_connections: 1,
    cpu_percent: 68,
    memory_mb: 42,
  },
  {
    id: "staging",
    type: "gateway",
    status: "offline",
    hostname: "staging.internal",
    version: "0.2.9",
    uptime_secs: 0,
    ws_connections: 0,
  },
];

const CONFIG_SCHEMA = {
  type: "object",
  description: "SeaClaw runtime configuration",
  properties: {
    default_provider: {
      type: "string",
      description: "Default AI model provider",
      enum: ["openai", "anthropic", "gemini", "ollama", "openrouter"],
      default: "openrouter",
    },
    default_model: {
      type: "string",
      description: "Default model identifier for chat completions",
      default: "claude-sonnet-4-20250514",
    },
    max_tokens: {
      type: "integer",
      description: "Maximum tokens per response",
      minimum: 100,
      maximum: 128000,
      default: 8192,
    },
    temperature: {
      type: "number",
      description: "Sampling temperature (0.0 = deterministic, 2.0 = creative)",
      minimum: 0,
      maximum: 2,
      default: 0.7,
    },
    system_prompt: {
      type: "string",
      description: "System prompt prepended to all conversations",
    },
    autonomy_level: {
      type: "string",
      description: "How much the agent can act without confirmation",
      enum: ["supervised", "semi", "full"],
      default: "semi",
    },
    sandbox_backend: {
      type: "string",
      description: "Sandbox for tool execution isolation",
      enum: ["none", "bubblewrap", "firejail", "landlock", "docker", "seatbelt"],
      default: "landlock",
    },
    log_level: {
      type: "string",
      description: "Logging verbosity",
      enum: ["debug", "info", "warn", "error"],
      default: "info",
    },
    memory_engine: {
      type: "string",
      description: "Memory storage backend",
      enum: ["sqlite", "markdown", "lru", "none"],
      default: "sqlite",
    },
    gateway_port: {
      type: "integer",
      description: "HTTP/WebSocket gateway port",
      default: 3000,
    },
    security: {
      type: "object",
      description: "Security and sandbox settings",
      properties: {
        allowed_domains: {
          type: "array",
          description: "Domains allowed for outbound requests",
          items: { type: "string" },
        },
        blocked_commands: {
          type: "array",
          description: "Shell commands that are never allowed",
          items: { type: "string" },
        },
      },
    },
  },
};

const DEMO_TOOLS = [
  { name: "shell", description: "Execute shell commands", category: "system" },
  { name: "file_read", description: "Read file contents", category: "filesystem" },
  { name: "file_write", description: "Write file contents", category: "filesystem" },
  { name: "git", description: "Git operations", category: "dev" },
  { name: "web_search", description: "Search the web", category: "web" },
  { name: "web_fetch", description: "Fetch URL contents", category: "web" },
];

const DEMO_CRON_RUNS = (() => {
  const nowSec = Math.floor(Date.now() / 1000);
  return [
    {
      id: 1001,
      automation_id: "1",
      name: "Daily Summary",
      started_at: nowSec - 3600,
      finished_at: nowSec - 3540,
      status: "completed",
      duration_ms: 60000,
      output: "Summarized 12 emails. 2 urgent items flagged.",
    },
    {
      id: 1002,
      automation_id: "1",
      name: "Daily Summary",
      started_at: nowSec - 90000,
      finished_at: nowSec - 89400,
      status: "completed",
      duration_ms: 60000,
      output: "No new emails overnight.",
    },
    {
      id: 1003,
      automation_id: "2",
      name: "Health Check",
      started_at: nowSec - 1800,
      finished_at: nowSec - 1795,
      status: "completed",
      duration_ms: 5000,
      output: "git pull: up to date. npm test: 3207 passed.",
    },
    {
      id: 1004,
      automation_id: "2",
      name: "Health Check",
      started_at: nowSec - 3600,
      finished_at: nowSec - 3592,
      status: "failed",
      duration_ms: 8000,
      output: "git pull failed: connection refused.",
    },
    {
      id: 1005,
      automation_id: "1",
      name: "Daily Summary",
      started_at: nowSec - 172800,
      finished_at: nowSec - 172740,
      status: "completed",
      duration_ms: 60000,
      output: "Summarized 8 emails. 1 calendar reminder.",
    },
    {
      id: 1006,
      automation_id: "2",
      name: "Health Check",
      started_at: nowSec - 5400,
      finished_at: nowSec - 5396,
      status: "completed",
      duration_ms: 4000,
      output: "All systems operational.",
    },
    {
      id: 1007,
      automation_id: "2",
      name: "Health Check",
      started_at: nowSec - 7200,
      finished_at: nowSec - 7190,
      status: "running",
      duration_ms: 10000,
      output: "",
    },
  ];
})();

export class DemoGatewayClient extends EventTarget {
  #status: GatewayStatus = "disconnected";
  #features: ServerFeatures = {};
  #interval: ReturnType<typeof setInterval> | null = null;
  #nextId = 100;

  private state = {
    sessions: makeSessions(),
    config: makeConfig() as Record<string, unknown>,
    cronJobs: makeCronJobs(),
    cronRuns: [...DEMO_CRON_RUNS],
    skills: makeSkills(),
    nodes: [...DEMO_NODES],
  };

  static readonly EVENT_STATUS = "status";
  static readonly EVENT_MESSAGE = "message";
  static readonly EVENT_GATEWAY = "gateway";

  get status(): GatewayStatus {
    return this.#status;
  }
  get features(): ServerFeatures {
    return this.#features;
  }

  connect(_url: string): void {
    this.#setStatus("connecting");
    setTimeout(() => {
      this.#setStatus("connected");
      this.#features = {
        methods: [],
        sessions: true,
        cron: true,
        skills: true,
        cost_tracking: true,
      };
      this.dispatchEvent(new CustomEvent("features", { detail: this.#features }));
      this.#seedInitialEvents();
      this.#startActivityStream();
    }, 400);
  }

  #setStatus(s: GatewayStatus): void {
    if (this.#status === s) return;
    this.#status = s;
    this.dispatchEvent(new CustomEvent(DemoGatewayClient.EVENT_STATUS, { detail: s }));
  }

  #seedInitialEvents(): void {
    const seed = [
      {
        event: "chat",
        payload: { channel: "Telegram", user: "Alice", preview: "PR review ready" },
      },
      { event: "tool_call", payload: { tool: "shell", command: "git status" } },
      { event: "health", payload: { status: "operational", uptime_secs: 172800 } },
      { event: "chat", payload: { channel: "Discord", user: "Bob", preview: "Deploy looks good" } },
      { event: "tool_call", payload: { tool: "web_search", command: "Rust async patterns" } },
      { event: "error", payload: { source: "email", message: "SMTP timeout" } },
    ];
    for (const s of seed) {
      this.dispatchEvent(new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, { detail: s }));
    }
  }

  #startActivityStream(): void {
    const channels = ["Telegram", "Discord", "Slack", "iMessage", "WhatsApp"];
    const users = ["Alice", "Bob", "Charlie", "Dana", "Eve"];
    const previews = [
      "Deployment completed successfully",
      "Can you check the logs?",
      "PR #42 merged",
      "Tests are green!",
      "New feature request from client",
      "Memory usage looks stable",
    ];
    let idx = 0;
    this.#interval = setInterval(() => {
      const event = {
        type: "message",
        channel: channels[idx % channels.length],
        user: users[idx % users.length],
        preview: previews[idx % previews.length],
        time: Date.now(),
      };
      this.dispatchEvent(
        new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, {
          detail: { event: "activity", payload: event },
        }),
      );
      idx++;
    }, 5000);
  }

  #emitChatResponse(userMessage: string, sessionKey?: string): void {
    const id = "demo-" + Date.now();
    const sk = sessionKey ?? "default";
    const fullResponse = `That's a great question about "${userMessage}". Here's what I think — the key insight is that well-designed systems tend to be modular and composable. Each piece does one thing well, and the connections between them are clean and predictable.`;
    const chunks = fullResponse.match(/.{1,12}/g) ?? [fullResponse];
    let delay = 400;

    const emitChunk = (content: string): void => {
      this.dispatchEvent(
        new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, {
          detail: {
            event: "chat",
            payload: { state: "chunk", message: content, id, session_key: sk },
          },
        }),
      );
    };

    for (const chunk of chunks) {
      setTimeout(() => emitChunk(chunk), delay);
      delay += 30 + Math.random() * 60;
    }

    setTimeout(() => {
      this.dispatchEvent(
        new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, {
          detail: {
            event: "chat",
            payload: { state: "sent", message: fullResponse, id, session_key: sk },
          },
        }),
      );
    }, delay + 100);
  }

  #handleRequest(method: string, params?: Record<string, unknown>): unknown {
    switch (method) {
      case "connect":
        return {
          type: "hello-ok",
          server: { version: "0.42.0" },
          features: { methods: [], sessions: true, cron: true, skills: true, cost_tracking: true },
        };
      case "health":
        return { status: "operational", uptime_secs: 172800 };
      case "capabilities":
        return { version: "0.42.0", tools: 53, channels: 20, providers: 50, peak_rss_mb: 5.9 };
      case "channels.status":
        return { channels: DEMO_CHANNELS };

      // --- Sessions (mutable) ---
      case "sessions.list":
        return { sessions: this.state.sessions };
      case "sessions.patch": {
        const key = params?.key as string;
        const label = params?.label as string;
        const s = this.state.sessions.find((x) => x.key === key);
        if (s && label) s.label = label;
        return { ok: true };
      }
      case "sessions.delete": {
        const key = params?.key as string;
        this.state.sessions = this.state.sessions.filter((x) => x.key !== key);
        return { ok: true };
      }

      case "update.check":
        return { available: false, current_version: "0.42.0" };
      case "chat.history":
        return {
          messages: [
            { role: "user", content: "Can you help me debug this memory leak?" },
            {
              role: "assistant",
              content:
                "Of course! Let me analyze the code. I'll start by checking the allocation patterns in the hot path.\n\nI found the issue — there's a missing `free()` call in the error branch of `sc_json_parse`. The allocated buffer leaks when parsing fails halfway through.",
            },
            { role: "user", content: "Great catch! Can you fix it?" },
            {
              role: "assistant",
              content:
                "Done. I've added the cleanup in the error path and added a regression test. All 2211 tests pass with 0 ASan errors.",
            },
          ],
        };
      case "activity.recent":
        return { events: DEMO_EVENTS };
      case "agents.list":
        return {
          agents: [
            {
              name: "main",
              status: "idle",
              model: "claude-sonnet-4-20250514",
              turns: 142,
              uptime: 86400,
            },
            { name: "researcher", status: "running", model: "gpt-4o", turns: 37, uptime: 3600 },
            { name: "coder", status: "idle", model: "gemini-2.5-pro", turns: 89, uptime: 7200 },
          ],
        };
      case "models.list":
        return {
          default_model: (this.state.config.default_model as string) ?? "claude-sonnet-4-20250514",
          providers: [
            {
              name: "openrouter",
              has_key: true,
              base_url: "https://openrouter.ai/api/v1",
              native_tools: true,
              is_default: (this.state.config.default_provider ?? "openrouter") === "openrouter",
            },
            {
              name: "anthropic",
              has_key: true,
              base_url: "https://api.anthropic.com",
              native_tools: true,
              is_default: (this.state.config.default_provider ?? "openrouter") === "anthropic",
            },
            {
              name: "openai",
              has_key: false,
              base_url: "https://api.openai.com/v1",
              native_tools: true,
              is_default: (this.state.config.default_provider ?? "openrouter") === "openai",
            },
            {
              name: "ollama",
              has_key: true,
              base_url: "http://localhost:11434",
              native_tools: false,
              is_default: (this.state.config.default_provider ?? "openrouter") === "ollama",
            },
            {
              name: "gemini",
              has_key: true,
              base_url: "https://generativelanguage.googleapis.com",
              native_tools: true,
              is_default: (this.state.config.default_provider ?? "openrouter") === "gemini",
            },
          ],
        };

      // --- Config (mutable) ---
      case "config.get":
        return { ...this.state.config };
      case "config.schema":
        return { schema: CONFIG_SCHEMA };
      case "config.set": {
        if (params) Object.assign(this.state.config, params);
        return { ok: true };
      }
      case "config.apply": {
        if (params) Object.assign(this.state.config, params);
        return { ok: true, applied: true };
      }

      case "tools.catalog":
        return { tools: DEMO_TOOLS };

      // --- Cron (mutable) ---
      case "cron.list":
        return { jobs: this.state.cronJobs };
      case "cron.runs": {
        const jobId = params?.id as number | undefined;
        const limit = (params?.limit as number) ?? 10;
        const filtered =
          jobId != null
            ? this.state.cronRuns.filter((r) => String(r.automation_id) === String(jobId))
            : this.state.cronRuns;
        return {
          runs: filtered.slice(0, limit).map((r) => ({
            id: r.id,
            started_at: r.started_at,
            finished_at: r.finished_at,
            status: r.status,
          })),
        };
      }
      case "cron.add": {
        const newId = this.#nextId++;
        const job = {
          id: newId,
          name: (params?.name as string) ?? "New Automation",
          type: (params?.type as string) ?? "agent",
          expression: (params?.schedule as string) ?? (params?.expression as string) ?? "0 * * * *",
          schedule: (params?.schedule as string) ?? (params?.expression as string) ?? "0 * * * *",
          prompt: (params?.prompt as string) ?? "",
          command: (params?.command as string) ?? "",
          enabled: true,
          paused: false,
          next_run: Math.floor(Date.now() / 1000) + 3600,
          last_run: 0,
          created_at: Math.floor(Date.now() / 1000),
        };
        this.state.cronJobs.push(job);
        return { ok: true, id: newId };
      }
      case "cron.update": {
        const id = params?.id as number;
        const job = this.state.cronJobs.find((j) => j.id === id);
        if (job) {
          if (params?.name != null) job.name = params.name as string;
          if (params?.schedule != null) {
            job.schedule = params.schedule as string;
            job.expression = params.schedule as string;
          }
          if (params?.prompt != null) job.prompt = params.prompt as string;
          if (params?.command != null) job.command = params.command as string;
          if (params?.enabled != null) {
            job.enabled = params.enabled as boolean;
            job.paused = !job.enabled;
          }
        }
        return { ok: true };
      }
      case "cron.remove": {
        const id = params?.id as number;
        this.state.cronJobs = this.state.cronJobs.filter((j) => j.id !== id);
        return { ok: true };
      }
      case "cron.run":
        return { ok: true };

      // --- Skills (mutable) ---
      case "skills.list":
        return { skills: this.state.skills };
      case "skills.search":
        return { entries: DEMO_REGISTRY };
      case "skills.enable": {
        const name = params?.name as string;
        const sk = this.state.skills.find((s) => s.name === name);
        if (sk) sk.enabled = true;
        return { ok: true };
      }
      case "skills.disable": {
        const name = params?.name as string;
        const sk = this.state.skills.find((s) => s.name === name);
        if (sk) sk.enabled = false;
        return { ok: true };
      }
      case "skills.install": {
        const name = (params?.name as string) ?? (params?.url as string) ?? "custom-skill";
        if (!this.state.skills.find((s) => s.name === name)) {
          this.state.skills.push({
            name,
            description: `Installed skill: ${name}`,
            enabled: true,
          });
        }
        return { ok: true };
      }
      case "skills.uninstall": {
        const name = params?.name as string;
        this.state.skills = this.state.skills.filter((s) => s.name !== name);
        return { ok: true };
      }
      case "skills.update":
        return { ok: true };

      case "metrics.snapshot":
        return {
          health: { uptime_seconds: 14523, pid: 12345 },
          metrics: {
            total_requests: 847,
            total_tokens: 1247832,
            total_tool_calls: 156,
            total_errors: 3,
            avg_latency_ms: 842,
            active_sessions: 2,
          },
          bth: {
            emotions_surfaced: 43,
            facts_extracted: 218,
            commitment_followups: 12,
            pattern_insights: 67,
            emotions_promoted: 31,
            events_extracted: 89,
            mood_contexts_built: 156,
            silence_checkins: 8,
            event_followups: 34,
            starters_built: 22,
            typos_applied: 15,
            corrections_sent: 7,
            thinking_responses: 45,
            callbacks_triggered: 19,
            reactions_sent: 28,
            link_contexts: 41,
            attachment_contexts: 13,
            ab_evaluations: 52,
            ab_alternates_chosen: 11,
            replay_analyses: 38,
            egraph_contexts: 67,
            vision_descriptions: 9,
            total_turns: 847,
          },
        };

      case "usage.summary":
        return {
          total_tokens: 1_247_832,
          total_cost: 18.42,
          session_cost_usd: 0.42,
          daily_cost_usd: 3.87,
          monthly_cost_usd: 18.42,
          request_count: 412,
          turns_today: 67,
          turns_week: 412,
          token_trend: [
            12000, 18400, 9200, 24100, 31200, 15800, 42300, 38100, 27600, 19400, 45200, 52100,
            33800, 28900, 47100, 39200, 21800, 54300, 41700, 36200, 29800, 48100, 57200, 43500,
          ],
          by_provider: [
            { provider: "openrouter", tokens: 892_100, cost: 12.3 },
            { provider: "anthropic", tokens: 245_732, cost: 4.92 },
            { provider: "ollama", tokens: 110_000, cost: 0 },
          ],
          daily_cost_history: [
            { date: "2026-03-01", cost: 1.83 },
            { date: "2026-03-02", cost: 2.14 },
            { date: "2026-03-03", cost: 3.07 },
            { date: "2026-03-04", cost: 2.51 },
            { date: "2026-03-05", cost: 2.89 },
            { date: "2026-03-06", cost: 2.11 },
            { date: "2026-03-07", cost: 3.87 },
          ],
          projected_monthly_usd: 81.53,
          previous_month_cost_usd: 72.4,
          cost_per_request: 0.045,
          tokens_per_turn: 18625,
          days_in_month: 31,
        };

      case "nodes.list":
        return { nodes: this.state.nodes };
      case "nodes.action":
        return { ok: true };

      // --- Chat (emits mock response) ---
      case "chat.send": {
        const msg = (params?.message as string) ?? "";
        const sk = (params?.sessionKey as string) ?? undefined;
        this.#emitChatResponse(msg, sk);
        return {};
      }
      case "chat.abort":
        return { aborted: true };

      case "voice.transcribe":
        return { text: "Demo transcription of your audio" };

      case "persona.set":
        return { ok: true };

      // --- OAuth mocks ---
      case "auth.oauth.start":
        return {
          url: "https://accounts.example.com/oauth?demo=true",
          state: "demo-state-" + Date.now(),
        };
      case "auth.oauth.callback":
        return {
          token: "demo-token-" + Date.now(),
          user: { id: "demo-user", email: "user@example.com" },
        };
      case "auth.oauth.refresh":
        return { access_token: "demo-refreshed-" + Date.now(), expires_in: 3600 };

      case "memory.status":
        return {
          engine: "sqlite",
          total_entries: 42,
          categories: { core: 8, daily: 18, conversation: 12, insight: 4 },
          last_consolidation: "2026-03-07T22:15:00Z",
        };
      case "memory.list":
        return {
          entries: [
            {
              id: "mem_1",
              key: "user-prefers-dark-mode",
              content: "User prefers dark mode and compact layouts",
              category: "core",
              source: "conversation:sess_abc",
              timestamp: "2026-03-01T10:00:00Z",
            },
            {
              id: "mem_2",
              key: "project-deadline-march",
              content: "Main project deadline is end of March 2026",
              category: "daily",
              source: "file://inbox/notes.txt",
              timestamp: "2026-03-05T14:30:00Z",
            },
            {
              id: "mem_3",
              key: "insight-preference-productivity",
              content: "User's preference for dark mode correlates with late-night work sessions",
              category: "insight",
              source: "connection_discovery",
              timestamp: "2026-03-07T22:15:00Z",
            },
          ],
        };
      case "memory.recall":
        return {
          entries: [
            {
              id: "mem_1",
              key: "user-prefers-dark-mode",
              content: "User prefers dark mode and compact layouts",
              category: "core",
              source: "conversation:sess_abc",
              timestamp: "2026-03-01T10:00:00Z",
              score: 0.92,
            },
          ],
        };
      case "memory.store":
        return { ok: true, id: "mem_new_" + Date.now() };
      case "memory.forget":
        return { ok: true, deleted: true };
      case "memory.ingest":
        return { ok: true, entries_created: 1 };
      case "memory.consolidate":
        return {
          ok: true,
          consolidated: 3,
          insights_generated: 1,
        };

      default:
        return {};
    }
  }

  async request<T = unknown>(method: string, params?: Record<string, unknown>): Promise<T> {
    await new Promise((r) => setTimeout(r, 80 + Math.random() * 200));
    return this.#handleRequest(method, params) as T;
  }

  abort(): Promise<{ aborted?: boolean }> {
    return Promise.resolve({ aborted: true });
  }

  disconnect(): void {
    if (this.#interval) clearInterval(this.#interval);
    this.#setStatus("disconnected");
  }
}
