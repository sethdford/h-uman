# src/tools/ — Tool Implementations

~90 tools that the agent can execute (registered in `factory.c`). Each tool implements the `hu_tool_t` vtable.

## Vtable Contract

Every tool must implement:

- `execute(ctx, params_json)` → `hu_tool_result_t`
- `name()` → stable lowercase string (e.g., `"shell"`)
- `description()` → human-readable description
- `parameters_json()` → JSON Schema for parameters

## Registration

All tools register in `factory.c`. Add your tool's header include and factory entry there.

## Tool Categories

### File System

```
file_read.c       Read file contents
file_write.c      Write/create files
file_edit.c       Edit file sections
file_append.c     Append to files
diff.c            Show file diffs
```

### Shell & Process

```
shell.c           Execute shell commands (sandboxed)
```

### Web & Network

```
http_request.c    Make HTTP requests
web_fetch.c       Fetch and parse web pages
web_search.c      Unified web search dispatcher
web_search_providers/
  brave.c         Brave Search
  duckduckgo.c    DuckDuckGo Search
  exa.c           Exa Search
  firecrawl.c     Firecrawl Search
  jina.c          Jina Search
  perplexity.c    Perplexity Search
  searxng.c       SearXNG Search
  tavily.c        Tavily Search
  common.c        Shared web search utilities
```

### Memory

```
memory_recall.c   Recall from memory
memory_store.c    Store to memory
memory_list.c     List memories
memory_forget.c   Remove memories
save_for_later.c  Save content for later retrieval
```

### Communication

```
send_message.c    Send message to user/channel
broadcast.c       Send to multiple channels
delegate.c        Delegate to another agent
```

### Agent Coordination

```
agent_spawn.c     Spawn sub-agents
agent_query.c     Query other agents
```

### Calendar & Scheduling

```
calendar.c        Calendar operations
cron_add.c        Add cron jobs
cron_remove.c     Remove cron jobs
cron_list.c       List cron jobs
cron_run.c        Run cron job manually
cron_update.c     Update cron jobs
cron_runs.c       View cron run history
```

### Business & External

```
crm.c             CRM operations
firebase.c        Firebase operations
gcloud.c          Google Cloud operations
facebook.c        Facebook API
instagram.c       Instagram API
twitter.c         Twitter/X API
analytics.c       Analytics operations
composio.c        Composio integration
jira.c            Jira issue operations
invoice.c         Invoice generation/parsing
social.c          Social media operations
spreadsheet.c     CSV/TSV spreadsheet operations
report.c          Structured report generation
workflow.c        DAG-based workflow execution
homeassistant.c   Home Assistant integration
```

### Code & Development

```
git.c             Git operations
lsp.c             Language Server Protocol operations
code_sandbox.c    Sandboxed code execution
claude_code.c     Claude Code integration
apply_patch.c     Apply code patches
```

### Skills & Agents

```
skill_run.c       Run installed skills
skill_write.c     Create/write skills
persona.c         Persona management
schedule.c        Scheduling operations
```

### Browser & GUI

```
browser.c         Browser automation (HU_ENABLE_TOOLS_BROWSER)
browser_open.c    Open URL in browser
computer_use.c    Computer use automation
gui_agent.c       GUI agent operations
screenshot.c      Take screenshots
canvas.c          Canvas rendering
notebook.c        Notebook operations
pwa.c             PWA bridge operations
```

### Data & Hardware

```
database.c        Database queries
pdf.c             PDF extraction
image.c           Image operations
hardware_info.c   Hardware information
hardware_memory.c Hardware memory operations
i2c.c             I2C bus operations (Linux)
spi.c             SPI bus operations (Linux)
peripheral_ctrl.c Peripheral control
```

### Utility

```
schema.c          JSON Schema operations
schema_clean.c    Schema cleanup utilities
pushover.c        Pushover notifications
message.c         Message formatting
validation.c      Input validation
path_security.c   Path security checks
spawn.c           Process spawning
cli_wrapper_common.c  CLI wrapper shared code
```

## Security Rules (High Risk)

- Validate ALL inputs before use — paths, URLs, commands, parameters
- Add `HU_IS_TEST` guard for tools that spawn processes or open network connections
- Never execute user-supplied strings as shell commands without sanitization
- URL tools: reject non-HTTPS schemes
- File tools: validate paths are within allowed directories
- Shell tool: runs through sandbox policy — never bypass

## Adding a New Tool

1. Create `src/tools/<name>.c` implementing `hu_tool_t`
2. Create `include/human/tools/<name>.h`
3. Register in `src/tools/factory.c`
4. Add tests in `tests/test_tools_all.c` or a dedicated test file
5. Add to CMakeLists.txt source list (with feature guard if optional)
