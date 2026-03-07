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

const DEMO_SESSIONS = [
  {
    key: "sess-a1",
    label: "Project Planning",
    created_at: Date.now() - 3600000,
    last_active: Date.now() - 120000,
    turn_count: 24,
  },
  {
    key: "sess-b2",
    label: "Code Review",
    created_at: Date.now() - 7200000,
    last_active: Date.now() - 600000,
    turn_count: 12,
  },
  {
    key: "sess-c3",
    label: "Bug Investigation",
    created_at: Date.now() - 86400000,
    last_active: Date.now() - 3600000,
    turn_count: 8,
  },
  {
    key: "sess-d4",
    label: "Architecture Discussion",
    created_at: Date.now() - 172800000,
    last_active: Date.now() - 86400000,
    turn_count: 31,
  },
  {
    key: "sess-e5",
    label: "Quick Question",
    created_at: Date.now() - 259200000,
    last_active: Date.now() - 172800000,
    turn_count: 3,
  },
];

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

function handleRequest(method: string, _params?: Record<string, unknown>): unknown {
  switch (method) {
    case "connect":
      return {
        type: "hello-ok",
        server: { version: "0.42.0" },
        features: { methods: [], sessions: true, cron: true, skills: true, cost_tracking: true },
      };
    case "health":
      return { status: "operational" };
    case "capabilities":
      return { version: "0.42.0", tools: 53, channels: 20, providers: 50 };
    case "channels.status":
      return { channels: DEMO_CHANNELS };
    case "sessions.list":
      return { sessions: DEMO_SESSIONS };
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
        default_model: "claude-sonnet-4-20250514",
        providers: [
          {
            name: "openrouter",
            has_key: true,
            base_url: "https://openrouter.ai/api/v1",
            native_tools: true,
            is_default: true,
          },
          {
            name: "anthropic",
            has_key: true,
            base_url: "https://api.anthropic.com",
            native_tools: true,
            is_default: false,
          },
          {
            name: "openai",
            has_key: false,
            base_url: "https://api.openai.com/v1",
            native_tools: true,
            is_default: false,
          },
          {
            name: "ollama",
            has_key: true,
            base_url: "http://localhost:11434",
            native_tools: false,
            is_default: false,
          },
          {
            name: "gemini",
            has_key: true,
            base_url: "https://generativelanguage.googleapis.com",
            native_tools: true,
            is_default: false,
          },
        ],
      };
    case "tools.list":
      return {
        tools: [
          { name: "shell", enabled: true, description: "Execute shell commands" },
          { name: "file_write", enabled: true, description: "Write content to files" },
          { name: "file_edit", enabled: true, description: "Edit existing files" },
          { name: "git", enabled: true, description: "Git operations" },
          { name: "web_search", enabled: true, description: "Search the web" },
          { name: "web_fetch", enabled: true, description: "Fetch URL content" },
          { name: "browser_open", enabled: true, description: "Open browser tabs" },
          { name: "memory_store", enabled: true, description: "Store to memory" },
          { name: "memory_recall", enabled: true, description: "Recall from memory" },
          { name: "cron_add", enabled: false, description: "Add scheduled tasks" },
          { name: "analytics", enabled: true, description: "Usage analytics" },
          { name: "screenshot", enabled: true, description: "Take screenshots" },
        ],
      };
    case "config.get":
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
    case "config.schema":
      return { schema: { type: "object", properties: {} } };
    case "config.set":
      return { ok: true };
    case "tools.catalog":
      return {
        tools: [
          { name: "shell", description: "Execute shell commands", category: "system" },
          { name: "file_read", description: "Read file contents", category: "filesystem" },
          { name: "file_write", description: "Write file contents", category: "filesystem" },
          { name: "git", description: "Git operations", category: "dev" },
          { name: "web_search", description: "Search the web", category: "web" },
          { name: "web_fetch", description: "Fetch URL contents", category: "web" },
        ],
      };
    case "cron.list":
      return {
        jobs: [
          {
            id: "j1",
            type: "agent",
            schedule: "0 9 * * *",
            prompt: "Summarize overnight emails",
            enabled: true,
            last_run: Date.now() - 3600000,
          },
          {
            id: "j2",
            type: "shell",
            schedule: "*/30 * * * *",
            command: "git pull && npm test",
            enabled: true,
            last_run: Date.now() - 1800000,
          },
          {
            id: "j3",
            type: "agent",
            schedule: "0 18 * * 1-5",
            prompt: "Generate daily standup report",
            enabled: false,
          },
        ],
      };
    case "skills.list":
      return {
        skills: [
          {
            name: "web-research",
            description: "Deep web research with source citations and fact-checking",
            enabled: true,
            parameters: '{"query": "string", "depth": "number"}',
          },
          {
            name: "code-review",
            description: "Automated code review with inline suggestions and severity ratings",
            enabled: true,
            parameters: '{"files": "string[]", "strictness": "string"}',
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
        ],
      };
    case "skills.search":
      return {
        entries: [
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
        ],
      };
    case "skills.enable":
    case "skills.disable":
    case "skills.install":
    case "skills.uninstall":
    case "skills.update":
      return { ok: true };
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
          12000, 18400, 9200, 24100, 31200, 15800, 42300, 38100, 27600, 19400, 45200, 52100, 33800,
          28900, 47100, 39200, 21800, 54300, 41700, 36200, 29800, 48100, 57200, 43500,
        ],
        by_provider: [
          { provider: "openrouter", tokens: 892_100, cost: 12.3 },
          { provider: "anthropic", tokens: 245_732, cost: 4.92 },
          { provider: "ollama", tokens: 110_000, cost: 0 },
        ],
      };
    case "nodes.list":
      return {
        nodes: [
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
        ],
      };
    case "nodes.action":
      return { ok: true };
    case "cron.runs":
      return { runs: [] };
    case "cron.add":
    case "cron.update":
    case "cron.remove":
      return { ok: true };
    case "cron.run":
      return { ok: true };
    case "sessions.patch":
    case "sessions.delete":
      return { ok: true };
    case "chat.send":
      return {};
    case "persona.set":
      return { ok: true };
    default:
      return {};
  }
}

export class DemoGatewayClient extends EventTarget {
  #status: GatewayStatus = "disconnected";
  #features: ServerFeatures = {};
  #interval: ReturnType<typeof setInterval> | null = null;

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

  async request<T = unknown>(method: string, params?: Record<string, unknown>): Promise<T> {
    await new Promise((r) => setTimeout(r, 80 + Math.random() * 200));
    return handleRequest(method, params) as T;
  }

  abort(): Promise<{ aborted?: boolean }> {
    return Promise.resolve({ aborted: true });
  }

  disconnect(): void {
    if (this.#interval) clearInterval(this.#interval);
    this.#setStatus("disconnected");
  }
}
