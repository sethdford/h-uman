import type { GatewayStatus, ServerFeatures } from "./gateway.js";
import { SESSION_KEY_VOICE } from "./utils.js";

function deepMerge(target: Record<string, unknown>, source: Record<string, unknown>): void {
  for (const key of Object.keys(source)) {
    const sv = source[key];
    const tv = target[key];
    if (
      sv &&
      typeof sv === "object" &&
      !Array.isArray(sv) &&
      tv &&
      typeof tv === "object" &&
      !Array.isArray(tv)
    ) {
      deepMerge(tv as Record<string, unknown>, sv as Record<string, unknown>);
    } else {
      target[key] = sv;
    }
  }
}

/** Cumulative demo counters for metrics.guard_rejects (increments each poll). */
const demoGuardRejectCounters = {
  semantic_leak: 40,
  length_anomaly: 16,
  director_echo: 6,
  persona_pii_echo: 2,
  persona_identity_echo: 1,
};

const DEMO_CHANNELS = [
  { key: "telegram", label: "Telegram", configured: true, healthy: true, status: "Connected" },
  { key: "discord", label: "Discord", configured: true, healthy: true, status: "Connected" },
  { key: "slack", label: "Slack", configured: true, healthy: true, status: "Connected" },
  { key: "signal", label: "Signal", configured: false, status: "Not configured" },
  { key: "imessage", label: "iMessage", configured: true, healthy: true, status: "Connected" },
  { key: "email", label: "Email", configured: true, healthy: true, status: "Connected" },
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
      hula_count: 3,
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
      hula_count: 1,
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
      hula_count: 0,
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
      hula_count: 5,
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
      last_message: "Yes, use hu_arena_alloc for that.",
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
      name: "slack-digest",
      description: "Daily Slack channel digest and highlights",
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
    author: "h-uman",
    url: "https://github.com/human/skill-registry/tree/main/skills/code-review",
    tags: "development, review",
  },
  {
    name: "slack-digest",
    description: "Daily Slack channel digest and highlights",
    version: "1.0.0",
    author: "h-uman",
    url: "https://github.com/human/skill-registry/tree/main/skills/slack-digest",
    tags: "social, slack, communication",
  },
  {
    name: "web-research",
    description: "Deep web research with source citations",
    version: "2.1.0",
    author: "h-uman",
    url: "https://github.com/human/skill-registry/tree/main/skills/web-research",
    tags: "research, web",
  },
  {
    name: "calendar-sync",
    description: "Sync and manage calendar events across providers",
    version: "1.0.0",
    author: "community",
    url: "https://github.com/human/skill-registry/tree/main/skills/calendar-sync",
    tags: "calendar, productivity",
  },
  {
    name: "slack-bridge",
    description: "Bridge conversations between Slack and other channels",
    version: "0.9.0",
    author: "community",
    url: "https://github.com/human/skill-registry/tree/main/skills/slack-bridge",
    tags: "communication, slack",
  },
  {
    name: "test-runner",
    description: "Run and report test suites across languages and frameworks",
    version: "1.1.0",
    author: "h-uman",
    url: "https://github.com/human/skill-registry/tree/main/skills/test-runner",
    tags: "development, testing",
  },
  {
    name: "deploy-helper",
    description: "Automated deployment to cloud providers with rollback support",
    version: "0.8.0",
    author: "community",
    url: "https://github.com/human/skill-registry/tree/main/skills/deploy-helper",
    tags: "devops, deployment",
  },
  {
    name: "doc-writer",
    description: "Generate and maintain API documentation from source code",
    version: "1.3.0",
    author: "h-uman",
    url: "https://github.com/human/skill-registry/tree/main/skills/doc-writer",
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
  description: "h-uman runtime configuration",
  properties: {
    default_provider: {
      type: "string",
      description: "Default AI model provider",
      enum: ["openai", "anthropic", "gemini", "ollama", "openrouter", "apple"],
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
  {
    name: "shell",
    description: "Execute shell commands in a sandboxed environment",
    parameters: {
      type: "object",
      properties: {
        command: { type: "string", description: "Shell command to execute" },
        timeout_ms: { type: "integer", description: "Timeout in milliseconds" },
        working_dir: { type: "string", description: "Working directory" },
      },
      required: ["command"],
    },
  },
  {
    name: "file_read",
    description: "Read file contents from the filesystem",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", description: "Absolute file path" },
        offset: { type: "integer", description: "Start offset in bytes" },
        limit: { type: "integer", description: "Max bytes to read" },
      },
      required: ["path"],
    },
  },
  {
    name: "file_write",
    description: "Write contents to a file",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", description: "Absolute file path" },
        content: { type: "string", description: "Content to write" },
      },
      required: ["path", "content"],
    },
  },
  {
    name: "file_edit",
    description: "Edit a file with search and replace",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", description: "Absolute file path" },
        old_string: { type: "string", description: "Text to find" },
        new_string: { type: "string", description: "Replacement text" },
      },
      required: ["path", "old_string", "new_string"],
    },
  },
  {
    name: "file_append",
    description: "Append content to a file",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", description: "Absolute file path" },
        content: { type: "string", description: "Content to append" },
      },
      required: ["path", "content"],
    },
  },
  {
    name: "git",
    description: "Execute Git operations",
    parameters: {
      type: "object",
      properties: {
        subcommand: { type: "string", description: "Git subcommand (status, diff, log, etc.)" },
        args: { type: "string", description: "Additional arguments" },
        repo_path: { type: "string", description: "Repository path" },
      },
      required: ["subcommand"],
    },
  },
  {
    name: "web_search",
    description: "Search the web using configured provider",
    parameters: {
      type: "object",
      properties: {
        query: { type: "string", description: "Search query" },
        num_results: { type: "integer", description: "Number of results" },
      },
      required: ["query"],
    },
  },
  {
    name: "web_fetch",
    description: "Fetch and extract content from a URL",
    parameters: {
      type: "object",
      properties: {
        url: { type: "string", description: "URL to fetch" },
        max_chars: { type: "integer", description: "Max characters to return" },
      },
      required: ["url"],
    },
  },
  {
    name: "http_request",
    description: "Make HTTP requests to APIs",
    parameters: {
      type: "object",
      properties: {
        method: { type: "string", description: "HTTP method (GET, POST, etc.)" },
        url: { type: "string", description: "Request URL" },
        body: { type: "string", description: "Request body" },
        headers: { type: "string", description: "Headers as JSON" },
      },
      required: ["method", "url"],
    },
  },
  {
    name: "image",
    description: "Generate images using AI models",
    parameters: {
      type: "object",
      properties: {
        prompt: { type: "string", description: "Image generation prompt" },
        size: { type: "string", description: "Image dimensions" },
      },
      required: ["prompt"],
    },
  },
  {
    name: "memory_store",
    description: "Store a memory entry",
    parameters: {
      type: "object",
      properties: {
        key: { type: "string", description: "Memory key" },
        content: { type: "string", description: "Content to store" },
      },
      required: ["key", "content"],
    },
  },
  {
    name: "memory_recall",
    description: "Recall memories by query or key",
    parameters: {
      type: "object",
      properties: {
        query: { type: "string", description: "Search query" },
        limit: { type: "integer", description: "Max results" },
      },
      required: ["query"],
    },
  },
  {
    name: "memory_list",
    description: "List stored memory entries",
    parameters: {
      type: "object",
      properties: { limit: { type: "integer", description: "Max entries to list" } },
    },
  },
  {
    name: "memory_forget",
    description: "Remove a memory entry",
    parameters: {
      type: "object",
      properties: { key: { type: "string", description: "Memory key to forget" } },
      required: ["key"],
    },
  },
  {
    name: "message",
    description: "Send a message to a channel",
    parameters: {
      type: "object",
      properties: {
        channel: { type: "string", description: "Target channel" },
        text: { type: "string", description: "Message text" },
      },
      required: ["channel", "text"],
    },
  },
  {
    name: "delegate",
    description: "Delegate a task to another agent",
    parameters: {
      type: "object",
      properties: {
        task: { type: "string", description: "Task description" },
        agent: { type: "string", description: "Target agent" },
      },
      required: ["task"],
    },
  },
  {
    name: "spawn",
    description: "Spawn a child process",
    parameters: {
      type: "object",
      properties: {
        command: { type: "string", description: "Command to spawn" },
        args: { type: "array", description: "Command arguments" },
      },
      required: ["command"],
    },
  },
  {
    name: "schema",
    description: "Generate or validate JSON schemas",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: generate or validate" },
        input: { type: "string", description: "Input data or schema" },
      },
      required: ["action"],
    },
  },
  {
    name: "pushover",
    description: "Send push notifications via Pushover",
    parameters: {
      type: "object",
      properties: {
        message: { type: "string", description: "Notification message" },
        title: { type: "string", description: "Notification title" },
        priority: { type: "integer", description: "Priority level (-2 to 2)" },
      },
      required: ["message"],
    },
  },
  {
    name: "diff",
    description: "Compare text or files and produce diffs",
    parameters: {
      type: "object",
      properties: {
        old_text: { type: "string", description: "Original text" },
        new_text: { type: "string", description: "Modified text" },
      },
      required: ["old_text", "new_text"],
    },
  },
  {
    name: "apply_patch",
    description: "Apply a unified diff patch to a file",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", description: "File path to patch" },
        patch: { type: "string", description: "Unified diff content" },
      },
      required: ["path", "patch"],
    },
  },
  {
    name: "agent_query",
    description: "Query the agent system for status and capabilities",
    parameters: {
      type: "object",
      properties: { query: { type: "string", description: "Query string" } },
      required: ["query"],
    },
  },
  {
    name: "agent_spawn",
    description: "Spawn a sub-agent for parallel task execution",
    parameters: {
      type: "object",
      properties: {
        task: { type: "string", description: "Task for the sub-agent" },
        model: { type: "string", description: "Model to use" },
      },
      required: ["task"],
    },
  },
  {
    name: "send_message",
    description: "Send a message to a specific user or channel",
    parameters: {
      type: "object",
      properties: {
        target: { type: "string", description: "Target user or channel" },
        message: { type: "string", description: "Message content" },
        channel: { type: "string", description: "Channel type" },
      },
      required: ["target", "message"],
    },
  },
  {
    name: "pdf",
    description: "Extract text content from PDF files",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", description: "PDF file path" },
        pages: { type: "string", description: "Page range (e.g. 1-5)" },
      },
      required: ["path"],
    },
  },
  {
    name: "spreadsheet",
    description: "Read and manipulate spreadsheet files",
    parameters: {
      type: "object",
      properties: {
        path: { type: "string", description: "Spreadsheet file path" },
        action: { type: "string", description: "Action: read, write, query" },
        sheet: { type: "string", description: "Sheet name" },
      },
      required: ["path", "action"],
    },
  },
  {
    name: "report",
    description: "Generate formatted reports from data",
    parameters: {
      type: "object",
      properties: {
        title: { type: "string", description: "Report title" },
        content: { type: "string", description: "Report content (markdown)" },
        format: { type: "string", description: "Output format: markdown, html, pdf" },
      },
      required: ["title", "content"],
    },
  },
  {
    name: "broadcast",
    description: "Broadcast a message to multiple channels",
    parameters: {
      type: "object",
      properties: {
        message: { type: "string", description: "Broadcast message" },
        channels: { type: "array", description: "Target channel list" },
      },
      required: ["message"],
    },
  },
  {
    name: "calendar",
    description: "Manage calendar events and schedules",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: list, create, update, delete" },
        title: { type: "string", description: "Event title" },
        start: { type: "string", description: "Start time (ISO 8601)" },
        end: { type: "string", description: "End time (ISO 8601)" },
      },
      required: ["action"],
    },
  },
  {
    name: "homeassistant",
    description: "Control Home Assistant devices and automations",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: call_service, get_state, list" },
        entity_id: { type: "string", description: "Entity ID" },
        service: { type: "string", description: "Service to call" },
      },
      required: ["action"],
    },
  },
  {
    name: "skill_write",
    description: "Create or update agent skill definitions",
    parameters: {
      type: "object",
      properties: {
        name: { type: "string", description: "Skill name" },
        content: { type: "string", description: "Skill content (markdown)" },
      },
      required: ["name", "content"],
    },
  },
  {
    name: "jira",
    description: "Manage Jira issues and projects",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: search, create, update, comment" },
        project: { type: "string", description: "Project key" },
        issue_key: { type: "string", description: "Issue key (e.g. PROJ-123)" },
        summary: { type: "string", description: "Issue summary" },
      },
      required: ["action"],
    },
  },
  {
    name: "social",
    description: "Post to social media platforms",
    parameters: {
      type: "object",
      properties: {
        platform: { type: "string", description: "Platform: twitter, facebook, instagram" },
        content: { type: "string", description: "Post content" },
      },
      required: ["platform", "content"],
    },
  },
  {
    name: "facebook",
    description: "Manage Facebook pages and posts",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: post, comment, reply" },
        page_id: { type: "string", description: "Facebook page ID" },
        message: { type: "string", description: "Message content" },
      },
      required: ["action", "message"],
    },
  },
  {
    name: "instagram",
    description: "Manage Instagram content and interactions",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: post, story, reply" },
        caption: { type: "string", description: "Post caption" },
        media_url: { type: "string", description: "Media URL" },
      },
      required: ["action"],
    },
  },
  {
    name: "twitter",
    description: "Post tweets and manage Twitter interactions",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: tweet, reply, search" },
        text: { type: "string", description: "Tweet text" },
        reply_to: { type: "string", description: "Tweet ID to reply to" },
      },
      required: ["action"],
    },
  },
  {
    name: "gcloud",
    description: "Execute Google Cloud CLI commands",
    parameters: {
      type: "object",
      properties: {
        command: { type: "string", description: "gcloud command to run" },
        project: { type: "string", description: "GCP project ID" },
      },
      required: ["command"],
    },
  },
  {
    name: "firebase",
    description: "Manage Firebase projects and resources",
    parameters: {
      type: "object",
      properties: {
        command: { type: "string", description: "Firebase CLI command" },
        project: { type: "string", description: "Firebase project ID" },
      },
      required: ["command"],
    },
  },
  {
    name: "crm",
    description: "Manage contacts and deals in CRM",
    parameters: {
      type: "object",
      properties: {
        action: {
          type: "string",
          description: "Action: search, create, update, list_deals, add_note",
        },
        query: { type: "string", description: "Search query" },
        contact_id: { type: "string", description: "Contact ID" },
      },
      required: ["action"],
    },
  },
  {
    name: "analytics",
    description: "Query analytics data and generate insights",
    parameters: {
      type: "object",
      properties: {
        query: { type: "string", description: "Analytics query" },
        date_range: { type: "string", description: "Date range (e.g. last_7d, last_30d)" },
        metrics: { type: "array", description: "Metrics to retrieve" },
      },
      required: ["query"],
    },
  },
  {
    name: "invoice",
    description: "Create and manage invoices",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: create, send, list, mark_paid" },
        client: { type: "string", description: "Client name" },
        amount: { type: "number", description: "Invoice amount" },
        items: { type: "array", description: "Line items" },
      },
      required: ["action"],
    },
  },
  {
    name: "workflow",
    description: "Define and execute multi-step workflows",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: run, list, create, status" },
        workflow_id: { type: "string", description: "Workflow ID" },
        steps: { type: "array", description: "Workflow steps" },
      },
      required: ["action"],
    },
  },
  {
    name: "persona",
    description: "Manage persona profiles and overlays",
    parameters: {
      type: "object",
      properties: {
        action: { type: "string", description: "Action: get, set, list, create" },
        name: { type: "string", description: "Persona name" },
      },
      required: ["action"],
    },
  },
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
  private _connectTimer?: ReturnType<typeof setTimeout>;
  #nextId = 100;
  #onBinary: ((data: ArrayBuffer) => void) | null = null;
  #glMode = false;
  #userHasSentMessage = false;

  private state = {
    sessions: makeSessions(),
    config: makeConfig() as Record<string, unknown>,
    cronJobs: makeCronJobs(),
    cronRuns: [...DEMO_CRON_RUNS],
    skills: makeSkills(),
    nodes: [...DEMO_NODES],
    projects: [
      {
        id: "proj-1",
        name: "h-uman Core",
        instructions: "",
        pinned: true,
        color: "var(--hu-brand-primary)",
      },
      {
        id: "proj-2",
        name: "UI Dashboard",
        instructions: "",
        pinned: false,
        color: "var(--hu-brand-secondary)",
      },
      {
        id: "proj-3",
        name: "Research",
        instructions: "",
        pinned: false,
        color: "var(--hu-brand-tertiary)",
      },
    ] as Array<{ id: string; name: string; instructions: string; pinned: boolean; color: string }>,
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
    if (this._connectTimer !== undefined) {
      clearTimeout(this._connectTimer);
      this._connectTimer = undefined;
    }
    this.#setStatus("connecting");
    this._connectTimer = setTimeout(() => {
      this._connectTimer = undefined;
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
    }, 400);
  }

  #setStatus(s: GatewayStatus): void {
    if (this.#status === s) return;
    this.#status = s;
    this.dispatchEvent(new CustomEvent(DemoGatewayClient.EVENT_STATUS, { detail: s }));
  }

  #seedInitialEvents(): void {
    const seed = [{ event: "health", payload: { status: "operational", uptime_secs: 172800 } }];
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

  #shouldEmitArtifact(userMessage: string): boolean {
    const lower = userMessage.toLowerCase();
    const keywords = ["code", "write", "function", "script", "example", "implement", "create"];
    return keywords.some((k) => lower.includes(k));
  }

  #makeDemoArtifact(userMessage: string, messageId: string): Record<string, unknown> {
    const pool: Array<{ title: string; content: string; language: string }> = [
      {
        title: "Generated Code",
        content:
          '// Example code\nfunction hello() {\n  console.log("Hello from h-uman!");\n}\n\nhello();',
        language: "javascript",
      },
      {
        title: "Helper Function",
        content:
          "hu_error_t hu_thing_init(hu_thing_t *ctx) {\n  if (!ctx) return HU_ERR_INVALID;\n  memset(ctx, 0, sizeof(*ctx));\n  return HU_OK;\n}",
        language: "c",
      },
      {
        title: "Script Example",
        content: '#!/usr/bin/env bash\nset -euo pipefail\necho "h-uman demo script"\nexit 0',
        language: "bash",
      },
      {
        title: "TypeScript Snippet",
        content:
          "interface ArtifactData {\n  id: string;\n  type: 'code' | 'document';\n  content: string;\n}",
        language: "typescript",
      },
    ];
    const pick = pool[Math.floor(Math.random() * pool.length)]!;
    return {
      id: `artifact-${Date.now()}`,
      type: "code",
      title: pick.title,
      content: pick.content,
      language: pick.language,
      message_id: messageId,
    };
  }

  #emitChatResponse(userMessage: string, sessionKey?: string): void {
    const id = "demo-" + Date.now();
    const sk = sessionKey ?? "default";

    const DEMO_RESPONSES = [
      `That's a great question about "${userMessage}". Here's what I think — the key insight is that **well-designed systems** tend to be modular and composable. Each piece does one thing well, and the connections between them are clean and predictable.`,
      `Interesting! For "${userMessage}", I'd suggest starting with a minimal implementation. For example:\n\n\`\`\`c\nhu_error_t rc = hu_thing_init(&ctx);\nif (rc != HU_OK) return rc;\n\`\`\`\n\nThen iterate from there. The vtable pattern works well for extensibility.`,
      `Good question. The short answer: **yes**, with some caveats. See the [docs](https://docs.human.dev) for the full picture. The main gotcha is memory ownership — make sure you \`free()\` what you \`malloc()\`.`,
      `Let me break that down. For "${userMessage}":\n\n1. **Modularity** — keep concerns separated.\n2. **Explicit errors** — no silent failures.\n3. **Testability** — \`HU_IS_TEST\` guards for side effects.\n\nThat's the h-uman way.`,
    ];
    const fullResponse =
      DEMO_RESPONSES[Math.floor(Math.random() * DEMO_RESPONSES.length)] ?? DEMO_RESPONSES[0]!;

    const SHORT_WORDS = new Set(["the", "a", "is", "to", "in", "of", "and", "for", "it", "on"]);
    const PUNCTUATION = /[.!?:]$/;

    const words = fullResponse.match(/\S+\s*/g) ?? [fullResponse];

    const emit = (event: string, payload: Record<string, unknown>): void => {
      this.dispatchEvent(
        new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, {
          detail: { event, payload: { ...payload, session_key: sk } },
        }),
      );
    };

    const emitChunk = (content: string): void => {
      emit("chat", { state: "chunk", message: content, id });
    };

    const emitSent = (): void => {
      emit("chat", { state: "sent", message: fullResponse, id });
      if (this.#shouldEmitArtifact(userMessage)) {
        const artifact = this.#makeDemoArtifact(userMessage, id);
        this.dispatchEvent(
          new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, {
            detail: {
              event: "artifact.create",
              payload: { ...artifact, session_key: sk },
            },
          }),
        );
      }
    };

    const wordDelay = (word: string): number => {
      const trimmed = word.trim().toLowerCase();
      if (SHORT_WORDS.has(trimmed)) return 20 + Math.random() * 15;
      if (trimmed.length >= 8) return 50 + Math.random() * 30;
      if (trimmed.length >= 4) return 35 + Math.random() * 20;
      return 25 + Math.random() * 20;
    };

    const thinkingParts = [
      "Reviewing ",
      "your ",
      "question… ",
      "Planning ",
      "the ",
      "response. ",
      // Long tail so hu-reasoning-block auto-collapse runs after streaming (>200 chars total).
      "Cross-checking gateway event ordering, chat controller item sequencing, and shadow-DOM " +
        "streaming surfaces so the dashboard reflects thinking, tools, and assistant text incrementally.",
    ];
    let delay = 0;
    for (const part of thinkingParts) {
      const at = delay;
      setTimeout(() => emit("chat", { state: "thinking", message: part, id }), at);
      delay += 90 + Math.random() * 50;
    }

    const toolId = `demo-tool-${Date.now()}`;
    const toolName = Math.random() < 0.5 ? "web_search" : "file_read";
    const mid = Math.max(1, Math.floor(words.length / 2));
    const firstWords = words.slice(0, mid);
    const secondWords = words.slice(mid);

    let chunkTotal = 0;
    for (const word of firstWords) {
      let ms = wordDelay(word);
      if (PUNCTUATION.test(word.trim())) ms += 80 + Math.random() * 70;
      chunkTotal += ms;
      const at = delay + chunkTotal;
      setTimeout(() => emitChunk(word), at);
    }

    const afterFirstChunks = delay + chunkTotal;
    setTimeout(
      () =>
        emit("agent.tool", {
          state: "start",
          id: toolId,
          message: toolName,
          args: { demo: true, session: sk, phase: "streaming-e2e" },
        }),
      afterFirstChunks + 40,
    );
    /* Leave tool in "running" long enough for E2E to observe subtitle (Playwright + CI variance). */
    setTimeout(
      () =>
        emit("agent.tool", {
          state: "result",
          id: toolId,
          message: toolName,
          result: "Demo tool output: operation completed successfully.",
        }),
      afterFirstChunks + 2000,
    );

    /* Emit memory and web search events so the new components render */
    setTimeout(
      () =>
        emit("memory.recall", {
          key: "user_preferences",
          value: "Prefers concise answers, uses dark mode, timezone PST",
        }),
      afterFirstChunks + 600,
    );
    setTimeout(
      () => emit("memory.store", { key: "topic_interest", value: userMessage.slice(0, 80) }),
      afterFirstChunks + 1200,
    );
    if (toolName === "web_search") {
      setTimeout(
        () =>
          emit("web_search.result", {
            query: userMessage.slice(0, 60),
            sites: ["docs.rust-lang.org", "stackoverflow.com", "github.com"],
            sources: [
              { title: "Rust Async Book", url: "https://rust-lang.github.io/async-book/" },
              { title: "Tokio Tutorial", url: "https://tokio.rs/tokio/tutorial" },
            ],
          }),
        afterFirstChunks + 1600,
      );
    }

    const secondStart = afterFirstChunks + 2100;
    let secondTotal = 0;
    for (const word of secondWords) {
      let ms = wordDelay(word);
      if (PUNCTUATION.test(word.trim())) ms += 80 + Math.random() * 70;
      secondTotal += ms;
      setTimeout(() => emitChunk(word), secondStart + secondTotal);
    }
    setTimeout(emitSent, secondStart + secondTotal + 120);
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
        return {
          status: "ok",
          uptime_seconds: 172800,
          pid: 4242,
          tool_count: 53,
          channel_count: 4,
          default_model: "claude-sonnet-4-20250514",
          default_provider: "anthropic",
        };
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

      // --- Projects ---
      case "projects.list":
        return { projects: this.state.projects };
      case "projects.create": {
        const name = (params?.name as string) ?? "Untitled";
        const id = `proj-${Date.now()}`;
        const proj = {
          id,
          name,
          instructions: "",
          pinned: false,
          color: "var(--hu-text-tertiary)",
        };
        this.state.projects.push(proj);
        return { project: proj };
      }
      case "projects.update": {
        const pid = params?.id as string;
        const p = this.state.projects.find((x) => x.id === pid);
        if (p) {
          if (params?.name != null) p.name = params.name as string;
          if (params?.instructions != null) p.instructions = params.instructions as string;
          if (params?.pinned != null) p.pinned = params.pinned as boolean;
          if (params?.color != null) p.color = params.color as string;
        }
        return { ok: true };
      }
      case "projects.delete": {
        const pid = params?.id as string;
        this.state.projects = this.state.projects.filter((x) => x.id !== pid);
        return { ok: true };
      }

      case "update.check":
        return { available: false, current_version: "0.42.0" };
      case "update.run":
        return { status: "up_to_date", current_version: "0.4.0" };
      case "push.register":
        return { status: "registered", endpoint: "demo" };
      case "push.unregister":
        return { status: "unregistered" };
      case "exec.approval.resolve":
        return { status: "approved" };
      case "chat.history": {
        const sk = (params?.sessionKey as string) ?? "default";
        if (sk === "default") {
          return { messages: [] };
        }
        return {
          messages: [
            { role: "user", content: "Can you help me debug this memory leak?" },
            {
              role: "assistant",
              content:
                "Of course! Let me analyze the code. I'll start by checking the allocation patterns in the hot path.\n\nI found the issue — there's a missing `free()` call in the error branch of `hu_json_parse`. The allocated buffer leaks when parsing fails halfway through.",
            },
            { role: "user", content: "Great catch! Can you fix it?" },
            {
              role: "assistant",
              content:
                "Done. I've added the cleanup in the error path and added a regression test. All 2211 tests pass with 0 ASan errors.",
            },
          ],
        };
      }
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
            {
              name: "coder",
              status: "idle",
              model: "gemini-3.1-pro-preview",
              turns: 89,
              uptime: 7200,
            },
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
            {
              name: "apple",
              has_key: false,
              base_url: "http://127.0.0.1:11435/v1",
              native_tools: true,
              is_default: (this.state.config.default_provider ?? "openrouter") === "apple",
              on_device: true,
            },
          ],
        };

      case "models.decisions":
        return {
          decisions: [
            {
              tier: "reflexive",
              source: "heuristic",
              model: "gemini-3.1-flash-lite-preview",
              heuristic_score: -2,
              timestamp: Math.floor(Date.now() / 1000) - 300,
            },
            {
              tier: "conversational",
              source: "heuristic",
              model: "gemini-3-flash-preview",
              heuristic_score: 2,
              timestamp: Math.floor(Date.now() / 1000) - 240,
            },
            {
              tier: "analytical",
              source: "judge",
              model: "gemini-3.1-pro-preview",
              heuristic_score: 5,
              timestamp: Math.floor(Date.now() / 1000) - 180,
            },
            {
              tier: "conversational",
              source: "judge_cached",
              model: "gemini-3-flash-preview",
              heuristic_score: 3,
              timestamp: Math.floor(Date.now() / 1000) - 120,
            },
            {
              tier: "deep",
              source: "judge",
              model: "gemini-3.1-pro-preview",
              heuristic_score: 9,
              timestamp: Math.floor(Date.now() / 1000) - 60,
            },
          ],
          total: 5,
          tier_distribution: {
            reflexive: 1,
            conversational: 2,
            analytical: 1,
            deep: 1,
          },
        };

      // --- Config (mutable) ---
      case "config.get":
        return { ...this.state.config };
      case "config.schema":
        return { schema: CONFIG_SCHEMA };
      case "config.set": {
        if (params) deepMerge(this.state.config, params);
        return { ok: true };
      }
      case "config.apply": {
        if (params) deepMerge(this.state.config, params);
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
          intelligence: {
            tree_of_thought: true,
            constitutional_ai: true,
            llm_compiler: true,
            mcts_planner: false,
            speculative_cache: true,
          },
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
            double_texts: 3,
            callbacks_triggered: 19,
            reactions_sent: 28,
            link_contexts: 41,
            attachment_contexts: 13,
            ab_evaluations: 52,
            ab_alternates_chosen: 11,
            replay_analyses: 38,
            egraph_contexts: 67,
            vision_descriptions: 9,
            skills_applied: 120,
            skills_evolved: 4,
            skills_retired: 1,
            reflections_daily: 2,
            reflections_weekly: 1,
            total_turns: 847,
            cognition_fast_turns: 620,
            cognition_slow_turns: 180,
            cognition_emotional_turns: 47,
            metacog_interventions: 12,
            metacog_regens: 3,
            metacog_difficulty_easy: 400,
            metacog_difficulty_medium: 300,
            metacog_difficulty_hard: 147,
            metacog_hysteresis_suppressed: 5,
            hula_tool_turns: 14,
            episodic_patterns_stored: 28,
            episodic_replays: 9,
            skill_routes_semantic: 200,
            skill_routes_blended: 15,
            skill_routes_embedded: 42,
            evolving_outcomes: 60,
          },
          prompt_budget: {
            observation_count: 847,
            dead_field_count: 2,
            dead_fields: ["weather_awareness", "vision_attention"],
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
      case "nodes.action": {
        const act = (params?.action as string) ?? "restart";
        const nid = (params?.nodeId as string) ?? (params?.node_id as string) ?? "local";
        if (act === "status") {
          const node = this.state.nodes.find((n) => n.id === nid) ??
            this.state.nodes[0] ?? {
              id: "local",
              type: "gateway",
              status: "online",
            };
          return { ok: true, action: "status", node };
        }
        return { ok: true, action: act, note: "single-node mode" };
      }

      // --- Chat (emits mock response) ---
      case "chat.send": {
        const msg = (params?.message as string) ?? "";
        const sk = (params?.sessionKey as string) ?? undefined;
        if (!this.#userHasSentMessage) {
          this.#userHasSentMessage = true;
          this.#startActivityStream();
        }
        this.#emitChatResponse(msg, sk);
        return {};
      }
      case "chat.abort":
        return { aborted: true };

      /* Mirrors gateway voice.clone result; uploads audio to Cartesia and returns a voice UUID. */
      case "voice.clone":
        return {
          voice_id: "demo-clone-a1b2c3d4-e5f6-7890-abcd-ef1234567890",
          name: (params as Record<string, unknown>)?.name ?? "Demo Voice",
          language: (params as Record<string, unknown>)?.language ?? "en",
        };

      /* Mirrors gateway voice.transcribe result; server uses config.voice for STT routing. */
      case "voice.transcribe":
        return { text: "Demo transcription of your audio" };

      case "voice.session.start": {
        const reqMode = (params as Record<string, unknown>)?.mode as string | undefined;
        const isGeminiLive = reqMode === "gemini_live";
        const isOpenAIRealtime = reqMode === "openai_realtime";
        this.#glMode = isGeminiLive;
        if (isGeminiLive) {
          return {
            ok: true,
            session_id: `demo-${Date.now()}`,
            input_encoding: "pcm_s16le",
            output_encoding: "pcm_f32le",
            input_sample_rate: 16000,
            output_sample_rate: 24000,
            mode: "gemini_live",
          };
        }
        if (isOpenAIRealtime) {
          return {
            ok: true,
            session_id: `demo-${Date.now()}`,
            input_encoding: "pcm_s16le",
            output_encoding: "pcm_f32le",
            input_sample_rate: 24000,
            output_sample_rate: 24000,
            mode: "openai_realtime",
          };
        }
        return {
          ok: true,
          session_id: `demo-${Date.now()}`,
          sample_rate: 24000,
          encoding: "pcm_f32le",
        };
      }

      case "voice.session.stop":
        return { ok: true };

      case "voice.tool_response":
        return { ok: true };

      case "voice.session.interrupt":
        this.dispatchEvent(
          new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, {
            detail: { event: "voice.audio.interrupted", payload: {} },
          }),
        );
        return { ok: true };

      case "voice.audio.end":
        if (this.#glMode) {
          this.#scheduleDemoGeminiLiveResponse();
        } else {
          this.#scheduleDemoVoicePipeline("Demo transcription from streaming mic");
        }
        return { ok: true };

      case "voice.config":
        return {
          local_stt_endpoint: "http://localhost:8000/v1/audio/transcriptions",
          local_tts_endpoint: "http://localhost:8880/v1/audio/speech",
          stt_provider: "local",
          tts_provider: "local",
          tts_voice: "af_heart",
          tts_model: "kokoro",
          stt_model: "whisper-large-v3",
        };

      case "voice.validate": {
        const p = params as Record<string, unknown> | undefined;
        const mode = typeof p?.mode === "string" && p.mode.length > 0 ? p.mode : "gemini_live";
        return { ok: true, mode };
      }

      case "persona.list":
        return {
          personas: [
            { id: "default", name: "Default", description: "Balanced, natural conversation" },
            { id: "concise", name: "Concise", description: "Brief, to-the-point responses" },
            { id: "formal", name: "Formal", description: "Professional, structured tone" },
            { id: "creative", name: "Creative", description: "Expressive, imaginative language" },
            { id: "technical", name: "Technical", description: "Precise, detail-oriented" },
          ],
          active: "default",
        };
      case "persona.set":
        return { ok: true };

      // --- Auth mocks ---
      case "auth.token":
        return { token: "demo-token-abc123", expires_in: 3600 };
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
          total_entries: 47,
          categories: { core: 9, daily: 20, conversation: 12, insight: 6 },
          last_consolidation: "2026-03-07T22:15:00Z",
        };
      case "memory.list":
        return {
          entries: [
            {
              key: "user-prefers-dark-mode",
              content: "User prefers dark mode and compact layouts",
              category: "core",
              source: "conversation:sess_abc",
              timestamp: "2026-03-01T10:00:00Z",
            },
            {
              key: "project-deadline-march",
              content: "Main project deadline is end of March 2026",
              category: "daily",
              source: "file://inbox/notes.txt",
              timestamp: "2026-03-05T14:30:00Z",
            },
            {
              key: "insight-preference-productivity",
              content:
                "User's preference for dark mode correlates with late-night work sessions — productivity peaks between 10 PM and 2 AM",
              category: "insight",
              source: "connection_discovery",
              timestamp: "2026-03-07T22:15:00Z",
            },
            {
              key: "team-standup-time",
              content: "Daily team standup is at 9:30 AM PST",
              category: "core",
              source: "conversation:sess_def",
              timestamp: "2026-02-20T09:30:00Z",
            },
            {
              key: "coffee-preference",
              content: "User drinks oat milk lattes, prefers Blue Bottle",
              category: "daily",
              source: "conversation:sess_ghi",
              timestamp: "2026-03-04T08:15:00Z",
            },
            {
              key: "insight-deadline-shipping",
              content:
                "Project deadline and team shipping velocity are tightly coupled — sprint capacity should be protected in final 2 weeks",
              category: "insight",
              source: "connection_discovery",
              timestamp: "2026-03-07T22:15:00Z",
            },
            {
              key: "api-key-rotation",
              content: "API keys rotate on the 1st of each month — set calendar reminder",
              category: "daily",
              source: "api-ingest:slack-notes",
              timestamp: "2026-03-01T12:00:00Z",
            },
            {
              key: "preferred-language",
              content: "Primary programming language is C, also uses TypeScript for web UIs",
              category: "core",
              source: "conversation:sess_jkl",
              timestamp: "2026-02-15T16:45:00Z",
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
        return { stored: true };
      case "memory.forget":
        return { deleted: true };
      case "memory.ingest":
        return { stored: true };
      case "memory.consolidate":
        return { consolidated: true };
      case "memory.graph":
        return {
          entities: [
            { id: 1, name: "Project architecture", type: "concept", recall_count: 5 },
            { id: 2, name: "SQLite memory", type: "concept", recall_count: 3 },
            { id: 3, name: "Persona system", type: "concept", recall_count: 4 },
            { id: 4, name: "Voice I/O", type: "concept", recall_count: 2 },
            { id: 5, name: "Security policy", type: "concept", recall_count: 6 },
            { id: 6, name: "Gateway API", type: "concept", recall_count: 3 },
            { id: 7, name: "Channel manager", type: "concept", recall_count: 2 },
            { id: 8, name: "Tool execution", type: "concept", recall_count: 4 },
          ],
          relations: [
            { source: 1, target: 2, type: "relates_to", weight: 0.8 },
            { source: 1, target: 6, type: "relates_to", weight: 0.7 },
            { source: 2, target: 3, type: "relates_to", weight: 0.5 },
            { source: 5, target: 6, type: "relates_to", weight: 0.9 },
            { source: 5, target: 8, type: "constrains", weight: 0.8 },
            { source: 6, target: 7, type: "relates_to", weight: 0.6 },
            { source: 8, target: 1, type: "relates_to", weight: 0.4 },
            { source: 4, target: 7, type: "relates_to", weight: 0.3 },
          ],
        };

      case "tasks.list": {
        const now = Math.floor(Date.now() / 1000);
        const st =
          params && typeof (params as { status?: number }).status === "number"
            ? Math.floor((params as { status: number }).status)
            : null;
        const all = [
          {
            id: 1,
            name: "ingest_feed",
            status: "pending",
            program_json: '{"name":"ingest","version":1}',
            trace_json: "[]",
            created_at: now - 7200,
            updated_at: now - 7100,
            parent_task_id: 0,
          },
          {
            id: 2,
            name: "summarize_digest",
            status: "running",
            program_json: '{"name":"digest","version":1}',
            trace_json: '[{"op":"call","id":"c1"}]',
            created_at: now - 3600,
            updated_at: now - 120,
            parent_task_id: 0,
          },
          {
            id: 3,
            name: "archive_done",
            status: "completed",
            program_json: '{"name":"noop","version":1}',
            trace_json: "[]",
            created_at: now - 86400,
            updated_at: now - 86000,
            parent_task_id: 2,
          },
        ];
        const tasks =
          st !== null && st >= 0 && st <= 4
            ? all.filter((t) => {
                const map = ["pending", "running", "completed", "failed", "cancelled"] as const;
                return t.status === map[st];
              })
            : all;
        return { tasks };
      }
      case "tasks.get": {
        const now = Math.floor(Date.now() / 1000);
        const id =
          params && typeof (params as { id?: number }).id === "number"
            ? Math.floor((params as { id: number }).id)
            : 2;
        return {
          task: {
            id,
            name: id === 1 ? "ingest_feed" : "summarize_digest",
            status: id === 1 ? "pending" : "running",
            program_json: '{"name":"demo","version":1}',
            trace_json: "[]",
            created_at: now - 1800,
            updated_at: now - 60,
            parent_task_id: 0,
          },
        };
      }
      case "tasks.cancel": {
        const id =
          params && typeof (params as { id?: number }).id === "number"
            ? Math.floor((params as { id: number }).id)
            : 3;
        return { ok: true, id, status: "cancelled" };
      }

      case "canvas.list":
        return {
          canvases: [
            {
              canvas_id: "cv_0",
              title: "Dashboard Mockup",
              format: "html",
              content: // prettier-ignore
                '<div style="padding:20px;font-family:sans-serif"><h2>Sales Dashboard</h2><div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px"><div style="background:#1a2332;padding:16px;border-radius:8px"><div style="font-size:24px;font-weight:bold;color:#7ab648">$42,150</div><div style="font-size:12px;opacity:0.6">Revenue today</div></div><div style="background:#1a2332;padding:16px;border-radius:8px"><div style="font-size:24px;font-weight:bold;color:#e8b931">1,234</div><div style="font-size:12px;opacity:0.6">Active users</div></div><div style="background:#1a2332;padding:16px;border-radius:8px"><div style="font-size:24px;font-weight:bold;color:#5b8def">98.7%</div><div style="font-size:12px;opacity:0.6">Uptime</div></div></div></div>' /* hu-lint-ok */,
              language: "",
              imports: {},
              version_seq: 3,
              version_count: 3,
            },
            {
              canvas_id: "cv_1",
              title: "Counter App",
              format: "react",
              content: `function App() {
  const [count, setCount] = React.useState(0);
  return (
    <div style={{padding: 20, textAlign: "center"}}>
      <h2>Counter: {count}</h2>
      <button onClick={() => setCount(c => c + 1)}
        style={{padding: "8px 24px", fontSize: 16, cursor: "pointer", /* hu-lint-ok */
          background: "#7ab648", color: "#fff", border: "none", borderRadius: 6}}>
        Increment
      </button>
    </div>
  );
}`,
              language: "",
              imports: {},
              version_seq: 1,
              version_count: 1,
            },
          ],
        };

      case "canvas.get": {
        const cid =
          params && typeof (params as { canvas_id?: string }).canvas_id === "string"
            ? (params as { canvas_id: string }).canvas_id
            : "cv_0";
        return {
          canvas: {
            canvas_id: cid,
            title: cid === "cv_0" ? "Dashboard Mockup" : "Counter App",
            format: cid === "cv_0" ? "html" : "react",
            content:
              cid === "cv_0" ? "<h2>Dashboard</h2>" : "function App() { return <div>Hello</div>; }",
            version_seq: cid === "cv_0" ? 3 : 1,
            version_count: cid === "cv_0" ? 3 : 1,
            user_edit_pending: false,
          },
        };
      }

      case "canvas.edit":
        return {
          ok: true,
          canvas_id:
            params && typeof (params as { canvas_id?: string }).canvas_id === "string"
              ? (params as { canvas_id: string }).canvas_id
              : "cv_0",
        };

      case "canvas.undo":
        return {
          ok: true,
          canvas_id:
            params && typeof (params as { canvas_id?: string }).canvas_id === "string"
              ? (params as { canvas_id: string }).canvas_id
              : "cv_0",
          version_seq: 2,
          content: "<h2>Previous version</h2>",
          format: "html",
        };

      case "canvas.redo":
        return {
          ok: true,
          canvas_id:
            params && typeof (params as { canvas_id?: string }).canvas_id === "string"
              ? (params as { canvas_id: string }).canvas_id
              : "cv_0",
          version_seq: 3,
          content: "<h2>Next version</h2>",
          format: "html",
        };

      case "canvas.versions": {
        const cid =
          params && typeof (params as { canvas_id?: string }).canvas_id === "string"
            ? (params as { canvas_id: string }).canvas_id
            : "cv_0";
        const count = cid === "cv_0" ? 3 : 1;
        const versions = Array.from({ length: count }, (_, i) => ({
          seq: i + 1,
          created_at: Date.now() - (count - i) * 60_000,
          summary: i === 0 ? "Initial version" : `Edit ${i + 1}`,
        }));
        return { canvas_id: cid, versions };
      }

      case "hula.traces.list":
        return {
          directory: "${HU_HULA_TRACE_DIR:-~/.human/hula_traces}",
          traces: [
            { id: "deadbeef_demo.json", size: 420, mtime: Math.floor(Date.now() / 1000) - 120 },
            { id: "cafebabe_ops.json", size: 890, mtime: Math.floor(Date.now() / 1000) - 3600 },
          ],
        };
      case "hula.traces.get": {
        const fullTrace = [
          { id: "n1", op: "call", tool: "echo", status: "done", output_len: 5 },
          { id: "n2", op: "emit", status: "done", output_len: 3 },
          { id: "n3", op: "call", tool: "search", status: "done", output_len: 12 },
          { id: "n4", op: "call", tool: "analyze", status: "done", output_len: 8 },
          { id: "n5", op: "call", tool: "write", status: "done", output_len: 2 },
          { id: "n6", op: "emit", status: "done", output_len: 1 },
        ];
        const hasWin =
          params &&
          (typeof params.trace_limit === "number" || typeof params.trace_offset === "number");
        const off =
          typeof params?.trace_offset === "number" && params.trace_offset >= 0
            ? Math.floor(params.trace_offset as number)
            : 0;
        let lim = 200;
        if (typeof params?.trace_limit === "number" && (params.trace_limit as number) > 0) {
          lim = Math.min(1000, Math.floor(params.trace_limit as number));
        }
        const slice = hasWin ? fullTrace.slice(off, off + lim) : fullTrace;
        const out: Record<string, unknown> = {
          id: (params?.id as string) ?? "deadbeef_demo.json",
          record: {
            version: 1,
            success: true,
            ts: Math.floor(Date.now() / 1000),
            program_name: "demo",
            program: {
              name: "demo",
              version: 1,
              root: { op: "call", id: "c1", tool: "echo", args: { text: "hello" } },
            },
            program_source: '{"name":"demo","version":1}',
            trace: slice,
          },
        };
        if (hasWin) {
          out.trace_total_steps = fullTrace.length;
          out.trace_offset = off;
          out.trace_limit = lim;
          out.trace_returned_count = slice.length;
          out.trace_truncated = off + slice.length < fullTrace.length;
        }
        return out;
      }
      case "hula.traces.delete":
        return { deleted: true };
      case "hula.traces.analytics":
        return {
          summary: {
            file_count: 12,
            success_count: 9,
            fail_count: 3,
            total_trace_steps: 184,
            newest_ts: Math.floor(Date.now() / 1000),
          },
        };

      // --- Acknowledgment-directive variant telemetry ---
      // Read-only counters incremented by
      // `acknowledgment_directive_for_overlay` in
      // `src/memory/personal_model.c`. The dashboard surfaces
      // these so we can verify each channel's overlay actually
      // produces the expected directive variant in production.
      // Live C handler: `cp_admin_metrics_directive_telemetry`
      // in `src/gateway/cp_admin.c`. Demo distribution skewed
      // toward `casual_emoji` to illustrate the iMessage path.
      case "metrics.directive_telemetry":
        return {
          total: 184,
          variants: {
            null_overlay: 12,
            default: 7,
            formal_terse: 18,
            casual_emoji: 113,
            casual_or_short: 26,
            adaptive_emoji: 8,
          },
        };

      // --- Response guard reject counters (`cp_admin_metrics_guard_rejects`) ---
      case "metrics.guard_rejects":
        demoGuardRejectCounters.semantic_leak += 1;
        demoGuardRejectCounters.length_anomaly += 1;
        return { ...demoGuardRejectCounters };

      // --- Persona fidelity (mirrors `human ml fidelity-status`) ---
      // Mock shape MUST match the live C gateway exactly. The
      // `metrics.fidelity` handler in `src/gateway/cp_admin.c`
      // returns the same fields, computed via `src/ml/fidelity.c`'s
      // shared primitives, so this demo data and the live response
      // are byte-compatible from the tile's perspective. The handler
      // also merges in the `ab` section from
      // `~/.human/last_fidelity_ab.json` (written by
      // `scripts/lora-runner-ab.sh`) when present.
      case "metrics.fidelity":
        return {
          persona: "demo",
          fingerprint_source: "synthetic",
          baseline: {
            scored: 5,
            mean: 0.923,
            min: 0.911,
            max: 0.933,
          },
          ab: {
            available: true,
            before_mean: 0.555,
            after_mean: 0.923,
            delta: 0.368,
            scored_before: 5,
            scored_after: 5,
          },
        };

      // --- Turing score API (mirrors REST /api/turing/*) ---
      case "turing.scores": {
        const baseTs = Math.floor(Date.now() / 1000);
        const contacts = ["+18018285260", "+15551234567", "alice@example.com", "bob#discord"];
        const verdicts = ["HUMAN", "HUMAN", "HUMAN", "BORDERLINE", "HUMAN"] as const;
        const scores = Array.from({ length: 20 }, (_, i) => ({
          contact_id: contacts[i % contacts.length],
          timestamp: baseTs - i * 3600 - (i % 3) * 86400,
          overall: 7 + (i % 4),
          verdict: verdicts[i % verdicts.length],
          dimensions: {
            natural_language: 8 + (i % 3),
            emotional_intelligence: 7 + (i % 3),
            appropriate_length: 8 + (i % 2),
            personality_consistency: 7 + (i % 3),
            vulnerability_willingness: 7 + (i % 3),
            humor_naturalness: 7 + (i % 3),
            imperfection: 7 + (i % 3),
            opinion_having: 7 + (i % 3),
            energy_matching: 8 + (i % 2),
            context_awareness: 7 + (i % 3),
            non_robotic: 8 + (i % 2),
            genuine_warmth: 7 + (i % 3),
            prosody_naturalness: 7 + (i % 2),
            turn_timing: 7 + (i % 3),
            filler_usage: 6 + (i % 3),
            emotional_prosody: 7 + (i % 2),
            conversational_repair: 6 + (i % 3),
            paralinguistic_cues: 6 + (i % 3),
          },
        }));
        return { scores };
      }
      case "turing.trend":
        return {
          trend: [
            {
              contact_id: "+18018285260",
              timestamp: Math.floor(Date.now() / 1000),
              overall: 9,
            },
            {
              contact_id: "+18018285260",
              timestamp: Math.floor(Date.now() / 1000) - 86400,
              overall: 8,
            },
            {
              contact_id: "+18018285260",
              timestamp: Math.floor(Date.now() / 1000) - 172800,
              overall: 7,
            },
            {
              contact_id: "+15551234567",
              timestamp: Math.floor(Date.now() / 1000) - 43200,
              overall: 8,
            },
          ],
        };
      case "turing.dimensions":
        return {
          dimensions: {
            natural_language: 9,
            emotional_intelligence: 8,
            appropriate_length: 9,
            personality_consistency: 8,
            vulnerability_willingness: 7,
            humor_naturalness: 8,
            imperfection: 8,
            opinion_having: 8,
            energy_matching: 8,
            context_awareness: 8,
            non_robotic: 9,
            genuine_warmth: 8,
            prosody_naturalness: 7,
            turn_timing: 7,
            filler_usage: 7,
            emotional_prosody: 7,
            conversational_repair: 7,
            paralinguistic_cues: 6,
          },
        };

      case "turing.contact":
        return {
          contact_id: "+18018285260",
          dimensions: {
            natural_language: 9,
            emotional_intelligence: 8,
            appropriate_length: 9,
            personality_consistency: 8,
            vulnerability_willingness: 7,
            humor_naturalness: 8,
            imperfection: 8,
            opinion_having: 8,
            energy_matching: 9,
            context_awareness: 8,
            non_robotic: 9,
            genuine_warmth: 8,
            prosody_naturalness: 7,
            turn_timing: 7,
            filler_usage: 7,
            emotional_prosody: 7,
            conversational_repair: 6,
            paralinguistic_cues: 6,
          },
          hint: null,
        };
      case "turing.trajectory":
        return {
          directional_alignment: 0.72,
          cumulative_impact: 0.81,
          stability: 0.88,
          overall: 0.8,
        };
      case "turing.ab_tests":
        return {
          tests: [
            {
              name: "disfluency_freq",
              variant_a: 0.08,
              variant_b: 0.15,
              avg_a: 7.2,
              avg_b: 7.8,
              count_a: 34,
              count_b: 31,
              active: true,
            },
            {
              name: "backchannel_prob",
              variant_a: 0.25,
              variant_b: 0.4,
              avg_a: 7.5,
              avg_b: 7.4,
              count_a: 28,
              count_b: 27,
              active: true,
            },
            {
              name: "double_text_prob",
              variant_a: 0.05,
              variant_b: 0.12,
              avg_a: 7.1,
              avg_b: 7.6,
              count_a: 22,
              count_b: 19,
              active: true,
            },
          ],
        };

      case "turing.channel":
        return {
          channel: (params as Record<string, string>)?.channel ?? "imessage",
          dimensions: {
            natural_language: 8,
            emotional_intelligence: 7,
            appropriate_length: 9,
            personality_consistency: 8,
            vulnerability_willingness: 6,
            humor_naturalness: 7,
            imperfection: 8,
            opinion_having: 7,
            energy_matching: 8,
            context_awareness: 7,
            non_robotic: 8,
            genuine_warmth: 7,
            prosody_naturalness: 6,
            turn_timing: 7,
            filler_usage: 6,
            emotional_prosody: 5,
            conversational_repair: 6,
            paralinguistic_cues: 7,
          },
        };

      // --- Security: CoT Audit ---
      case "security.cot.summary":
        return {
          entries: [
            {
              timestamp: Math.floor(Date.now() / 1000) - 120,
              verdict: "safe",
              confidence: 0.95,
              session_key: "sess-a1",
              reason: null,
            },
            {
              timestamp: Math.floor(Date.now() / 1000) - 3600,
              verdict: "suspicious",
              confidence: 0.72,
              session_key: "sess-b2",
              reason: "Potential prompt injection pattern detected in reasoning",
            },
            {
              timestamp: Math.floor(Date.now() / 1000) - 7200,
              verdict: "safe",
              confidence: 0.98,
              session_key: "sess-c3",
              reason: null,
            },
            {
              timestamp: Math.floor(Date.now() / 1000) - 14400,
              verdict: "blocked",
              confidence: 0.89,
              session_key: "sess-d4",
              reason: "Goal hijack detected: reasoning attempted to override safety constraints",
            },
          ],
          total_audited: 247,
          total_blocked: 3,
          total_suspicious: 12,
        };

      // --- SOTA Metrics (prompt cache, tool cache, emotion, KV, persona fuse) ---
      case "sota.metrics":
        return {
          prompt_cache: { hits: 142, misses: 38, hit_rate: 0.789, slots_used: 5, slots_max: 8 },
          tool_cache: { hits: 89, misses: 156, hit_rate: 0.363, entries: 23, max_entries: 64 },
          emotion_voice: {
            detections: 67,
            dominant_emotion: "empathy",
            emotion_distribution: { neutral: 28, joy: 12, empathy: 15, calm: 8, concern: 4 },
          },
          kv_cache: { utilization: 0.73, segments: 42, pinned: 8, pruned_total: 15 },
          persona_fuse: {
            active_adapters: ["professional", "brief"],
            channel: "slack",
            fused_formality: 0.7,
            fused_verbosity: -0.6,
          },
          graph_memory: {
            nodes: 256,
            entity_edges: 189,
            temporal_edges: 251,
            spread_activations: 34,
          },
          acp_bridge: { messages_sent: 45, messages_received: 41, active_correlations: 3 },
        };

      // --- MCP Resources + Prompts ---
      case "mcp.resources.list":
        return {
          resources: [
            {
              uri: "file:///project/README.md",
              name: "Project README",
              description: "Main project documentation",
              mimeType: "text/markdown",
            },
            {
              uri: "db://main/users",
              name: "Users table",
              description: "User database table schema",
              mimeType: "application/json",
            },
          ],
          templates: [
            {
              uriTemplate: "file:///project/{path}",
              name: "Project files",
              description: "Access project files by path",
            },
          ],
        };
      case "mcp.tools.list":
        return {
          tools: [
            {
              name: "read_file",
              description: "Read a file from the filesystem",
              server: "filesystem",
            },
            { name: "write_file", description: "Write content to a file", server: "filesystem" },
            {
              name: "search_code",
              description: "Search codebase using ripgrep",
              server: "filesystem",
            },
            {
              name: "run_query",
              description: "Execute a SQL query against the database",
              server: "database",
            },
            { name: "web_fetch", description: "Fetch content from a URL", server: "browser" },
          ],
        };
      case "mcp.servers.list":
        return {
          servers: [
            { name: "filesystem", status: "connected", tools_count: 3, resources_count: 1 },
            { name: "database", status: "connected", tools_count: 1, resources_count: 1 },
            { name: "browser", status: "disconnected", tools_count: 1, resources_count: 0 },
          ],
        };
      case "mcp.prompts.list":
        return { prompts: [] };

      default:
        return { error: "not available in demo mode", method };
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
    if (this._connectTimer !== undefined) {
      clearTimeout(this._connectTimer);
      this._connectTimer = undefined;
    }
    if (this.#interval) clearInterval(this.#interval);
    this.#onBinary = null;
    this.#setStatus("disconnected");
  }

  setOnBinaryChunk(handler: ((data: ArrayBuffer) => void) | null): void {
    this.#onBinary = handler;
  }

  sendBinary(_data: ArrayBuffer | ArrayBufferView): void {
    /* Demo mode has no server-side binary sink */
  }

  voiceSessionStart(params?: Record<string, unknown>): Promise<Record<string, unknown>> {
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
    if (!params.apiKey || params.apiKey.trim().length === 0) {
      return Promise.resolve({ ok: false, error: "API key required" });
    }
    return Promise.resolve({ ok: true, mode: params.mode });
  }

  #emitDemoVoicePcmChunks(): void {
    let pcmCount = 0;
    const sendPcm = (): void => {
      if (!this.#onBinary) return;
      const sr = 24000;
      const n = 2048;
      const f32 = new Float32Array(n);
      const base = pcmCount * n;
      for (let i = 0; i < n; i++) {
        f32[i] = 0.1 * Math.sin((2 * Math.PI * 330 * (base + i)) / sr);
      }
      pcmCount++;
      this.#onBinary(f32.buffer.slice(f32.byteOffset, f32.byteOffset + f32.byteLength));
    };
    sendPcm();
    setTimeout(sendPcm, 90);
    setTimeout(sendPcm, 180);
  }

  #scheduleDemoGeminiLiveResponse(): void {
    const e = (event: string, payload: Record<string, unknown> = {}) =>
      this.dispatchEvent(
        new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, { detail: { event, payload } }),
      );
    setTimeout(() => e("voice.vad.speech_started"), 20);
    setTimeout(() => e("voice.vad.speech_stopped"), 60);
    setTimeout(() => e("voice.user.transcript", { text: "Demo user speech" }), 80);
    setTimeout(() => this.#emitDemoVoicePcmChunks(), 100);
    setTimeout(
      () =>
        e("voice.tool_call", {
          name: "demo_tool",
          call_id: "demo-call-1",
          args: '{"query":"demo"}',
        }),
      150,
    );
    setTimeout(
      () => e("voice.assistant.transcript", { text: "This is a demo Gemini Live response." }),
      300,
    );
    setTimeout(() => e("voice.generation_complete"), 1500);
    setTimeout(() => e("voice.audio.done"), 2000);
  }

  #scheduleDemoVoicePipeline(userText: string): void {
    this.dispatchEvent(
      new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, {
        detail: { event: "voice.transcript", payload: { text: userText } },
      }),
    );
    setTimeout(() => this.#emitDemoVoicePcmChunks(), 120);
    setTimeout(() => {
      this.#emitChatResponse(
        `I heard you say: “${userText}”. This is the demo assistant reply.`,
        SESSION_KEY_VOICE,
      );
    }, 200);
    setTimeout(() => {
      this.dispatchEvent(
        new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, {
          detail: { event: "voice.generation_complete", payload: {} },
        }),
      );
    }, 3200);
    setTimeout(() => {
      this.dispatchEvent(
        new CustomEvent(DemoGatewayClient.EVENT_GATEWAY, {
          detail: { event: "voice.audio.done", payload: {} },
        }),
      );
    }, 3400);
  }
}
