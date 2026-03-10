# Human Skill Registry

The skill registry is a JSON index of publishable skills. Users run `human skills search`, `install`, and `update` against this index.

## registry.json Format

Array of skill entries:

```json
[
  {
    "name": "code-review",
    "description": "Automated code review with style checks and security scanning",
    "version": "1.0.0",
    "author": "human",
    "url": "https://github.com/human/skill-registry/tree/main/skills/code-review",
    "tags": ["development", "review"]
  }
]
```

### Fields

| Field         | Type             | Required | Description                        |
| ------------- | ---------------- | -------- | ---------------------------------- |
| `name`        | string           | yes      | Unique skill name                  |
| `description` | string           | yes      | Short description                  |
| `version`     | string           | yes      | Semantic version                   |
| `author`      | string           | yes      | Author or org                      |
| `url`         | string           | yes      | GitHub tree URL to skill directory |
| `tags`        | array of strings | optional | Categories for search              |

### URL Format

The `url` must be a GitHub **tree** URL, e.g.:

```
https://github.com/human/skill-registry/tree/main/skills/code-review
```

Human converts this to raw content URLs when installing, e.g.:

```
https://raw.githubusercontent.com/human/skill-registry/main/skills/code-review/code-review.skill.json
```

## Publishing a Skill

1. **Fork** the [human/skill-registry](https://github.com/human/skill-registry) repo.
2. **Add** your skill under `skills/<skill-name>/`:
   - `<skill-name>.skill.json`
   - `SKILL.md`
3. **Append** an entry to `registry.json` at the repo root.
4. **Open a PR** to `main`.

## CI Validation Requirements

The registry repo CI must:

1. **Validate** all `*.skill.json` files have required fields: `name`, `description`, `version`, `author`, `url`.
2. **Validate** `registry.json` is valid JSON.
3. **Validate** every entry in `registry.json` references an existing skill directory under `skills/<name>/` with a matching `<name>.skill.json`.

See `.github/workflows/validate.yml` in the skill-registry repo.
