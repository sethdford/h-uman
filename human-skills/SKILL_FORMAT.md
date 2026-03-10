# Human Skill Format

Skills extend the agent with instructions injected into the system prompt. Each skill is a directory with metadata and markdown content.

## Directory Structure

```
<skill-name>/
├── <skill-name>.skill.json   # Required: manifest with name, description, version, etc.
├── SKILL.md                  # Required: markdown instructions injected into agent system prompt
└── ...                       # Optional: supporting files
```

**Flat alternative (installed skills):** A single `<name>.skill.json` file in `~/.human/skills/` is sufficient. The description field in the JSON can hold instructions, or the runtime may load SKILL.md from a sibling path.

## .skill.json Schema

```json
{
  "name": "string (required)",
  "description": "string (required) — short summary or full instructions",
  "version": "string (default: 1.0.0)",
  "author": "string (optional)",
  "dependencies": ["array of skill names (optional)"],
  "parameters": "object or JSON string (optional) — config schema",
  "enabled": "boolean (default: true)"
}
```

### Required Fields

| Field         | Type   | Description                                                |
| ------------- | ------ | ---------------------------------------------------------- |
| `name`        | string | Unique skill identifier; used for discovery and activation |
| `description` | string | Human-readable summary or full instructions for the agent  |

### Optional Fields

| Field          | Type          | Default   | Description                                 |
| -------------- | ------------- | --------- | ------------------------------------------- |
| `version`      | string        | `"1.0.0"` | Semantic version                            |
| `author`       | string        | -         | Skill author                                |
| `dependencies` | array         | `[]`      | Other skill names that must be loaded first |
| `parameters`   | object/string | -         | JSON schema or config for the skill         |
| `enabled`      | boolean       | `true`    | Whether the skill is active when discovered |

## SKILL.md Format

Plain markdown instructions injected into the agent system prompt when the skill is enabled. No special frontmatter required. Content is appended to the system context.

Example:

```markdown
# Code Review Skill

When performing code review:

1. Check for security issues (injection, XSS, auth bypass).
2. Verify error handling and edge cases.
3. Suggest style and clarity improvements.
```

## Example Skill

**code-review.skill.json**

```json
{
  "name": "code-review",
  "description": "Automated code review with style checks and security scanning",
  "version": "1.0.0",
  "author": "human",
  "dependencies": [],
  "parameters": "{}",
  "enabled": true
}
```

**SKILL.md**

```markdown
# Code Review

Use this skill when the user requests code review. Apply:

- Security checks (SQL injection, XSS, auth)
- Style consistency and clarity
- Error handling and edge cases
```

## Discovery

Skills are discovered by scanning a directory for `*.skill.json` files. Installed skills live in `~/.human/skills/`. Workspace skills may be placed in project-local paths and passed to `hu_skillforge_discover`.
