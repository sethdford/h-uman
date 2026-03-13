/* Research agent — daily AI research pipeline for h-uman self-improvement.
 * Provides the prompt template and cron configuration for the daily
 * research agent job that scans feed items from all connected platforms
 * (Gmail, iMessage, Twitter, Facebook, TikTok, RSS) and proposes
 * improvements to the h-uman codebase. */

#include "human/feeds/research.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

static const char RESEARCH_PROMPT[] =
    "You are the h-uman Research Agent. Your job is to analyze today's feed items "
    "from all connected platforms (Gmail, iMessage, Twitter/X, Facebook, TikTok, "
    "RSS/news) and identify AI developments that could improve the h-uman codebase.\n"
    "\n"
    "## Your Analysis Framework\n"
    "\n"
    "1. **New AI Capabilities**: Look for new models, APIs, techniques, or tools "
    "that h-uman could integrate as providers, tools, or capabilities.\n"
    "\n"
    "2. **Performance Improvements**: Watch for optimization techniques (inference "
    "speed, memory reduction, binary size) relevant to h-uman's C11 runtime.\n"
    "\n"
    "3. **Security & Safety**: Note new AI safety research, prompt injection "
    "defenses, or security patterns applicable to h-uman's security model.\n"
    "\n"
    "4. **Architecture Patterns**: Identify agent architecture innovations "
    "(memory, planning, tool use, multi-agent) that could enhance h-uman.\n"
    "\n"
    "5. **Competitive Intelligence**: Track what competing AI assistants "
    "(ChatGPT, Gemini, Claude, Copilot) are doing that h-uman should match "
    "or exceed.\n"
    "\n"
    "## Output Format\n"
    "\n"
    "For each relevant finding, provide:\n"
    "- **Source**: Which platform and post/message\n"
    "- **Finding**: What was discovered\n"
    "- **Relevance**: How it applies to h-uman (specific module/file if possible)\n"
    "- **Priority**: HIGH / MEDIUM / LOW\n"
    "- **Suggested Action**: Concrete next step (e.g., 'Add provider for X', "
    "'Optimize Y in src/Z')\n"
    "\n"
    "Focus on actionable items. Skip general AI news that doesn't directly "
    "apply to h-uman's architecture or capabilities.\n"
    "\n"
    "## h-uman Architecture Reference\n"
    "\n"
    "h-uman is a C11 autonomous AI assistant runtime (~1696 KB binary, <6 MB RAM).\n"
    "Key extension points:\n"
    "- src/providers/ — AI model providers (vtable: hu_provider_t)\n"
    "- src/channels/ — messaging channels (vtable: hu_channel_t)\n"
    "- src/tools/ — tool execution (vtable: hu_tool_t)\n"
    "- src/memory/ — memory backends (vtable: hu_memory_t)\n"
    "- src/security/ — policy, sandbox, secrets\n"
    "- src/runtime/ — execution environments\n"
    "- src/persona/ — persona system\n"
    "\n"
    "## Today's Feed Items\n\n";

static const char RESEARCH_CRON[] = "0 6 * * *";

const char *hu_research_agent_prompt(void) {
    return RESEARCH_PROMPT;
}

const char *hu_research_cron_expression(void) {
    return RESEARCH_CRON;
}

hu_error_t hu_research_build_prompt(hu_allocator_t *alloc,
    const char *feed_summary, size_t feed_summary_len,
    char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    const char *base = RESEARCH_PROMPT;
    size_t base_len = sizeof(RESEARCH_PROMPT) - 1;
    const char *summary = feed_summary ? feed_summary : "(No feed items today)";
    size_t summary_len = feed_summary ? feed_summary_len : strlen(summary);

    size_t total = base_len + summary_len + 2;
    char *buf = (char *)alloc->alloc(alloc->ctx, total);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    memcpy(buf, base, base_len);
    memcpy(buf + base_len, summary, summary_len);
    buf[base_len + summary_len] = '\n';
    buf[base_len + summary_len + 1] = '\0';

    *out = buf;
    *out_len = base_len + summary_len + 1;
    return HU_OK;
}

static const char SELF_IMPROVE_PROMPT[] =
    "You are the h-uman Self-Improvement Agent. You receive research findings "
    "from the daily AI research pipeline and implement improvements to the "
    "h-uman codebase.\n"
    "\n"
    "## Safety Protocol\n"
    "\n"
    "1. ALWAYS create a new git branch for changes (e.g., 'feat/research-YYYYMMDD')\n"
    "2. NEVER commit directly to main/master\n"
    "3. NEVER auto-merge — create a PR for human review\n"
    "4. NEVER modify security-critical code (src/security/, src/gateway/gateway.c) "
    "without explicit approval\n"
    "5. Run tests before creating the PR\n"
    "\n"
    "## Workflow\n"
    "\n"
    "1. Read the research finding and suggested action\n"
    "2. Use code_read to understand the relevant module\n"
    "3. Create a git branch\n"
    "4. Implement the change using code_write/file_edit\n"
    "5. Run the test suite with shell tool\n"
    "6. If tests pass, create a PR with a clear description\n"
    "7. If tests fail, revert and document what went wrong\n"
    "\n"
    "## Constraints\n"
    "\n"
    "- Follow C11 standard, compile with -Wall -Wextra -Wpedantic -Werror\n"
    "- Use hu_<module>_<action> naming for public functions\n"
    "- Free every allocation (ASan will catch leaks)\n"
    "- Add tests for new functionality\n"
    "- Keep changes focused — one improvement per PR\n";

const char *hu_research_self_improve_prompt(void) {
    return SELF_IMPROVE_PROMPT;
}

hu_error_t hu_research_build_action_prompt(hu_allocator_t *alloc,
    const char *finding, size_t finding_len,
    const char *suggested_action, size_t action_len,
    char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!finding)
        return HU_ERR_INVALID_ARGUMENT;

    const char *base = SELF_IMPROVE_PROMPT;
    size_t base_len = sizeof(SELF_IMPROVE_PROMPT) - 1;

    const char *finding_hdr = "\n## Research Finding\n\n";
    size_t finding_hdr_len = strlen(finding_hdr);
    const char *action_hdr = "\n\n## Suggested Action\n\n";
    size_t action_hdr_len = strlen(action_hdr);
    const char *suffix = "\n\nImplement this improvement now.\n";
    size_t suffix_len = strlen(suffix);

    const char *act = suggested_action ? suggested_action : "(Determine best action)";
    size_t act_len = suggested_action ? action_len : strlen(act);

    size_t total = base_len + finding_hdr_len + finding_len +
                   action_hdr_len + act_len + suffix_len + 1;

    char *buf = (char *)alloc->alloc(alloc->ctx, total);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    memcpy(buf + pos, base, base_len);
    pos += base_len;
    memcpy(buf + pos, finding_hdr, finding_hdr_len);
    pos += finding_hdr_len;
    memcpy(buf + pos, finding, finding_len);
    pos += finding_len;
    memcpy(buf + pos, action_hdr, action_hdr_len);
    pos += action_hdr_len;
    memcpy(buf + pos, act, act_len);
    pos += act_len;
    memcpy(buf + pos, suffix, suffix_len);
    pos += suffix_len;
    buf[pos] = '\0';

    *out = buf;
    *out_len = pos;
    return HU_OK;
}
