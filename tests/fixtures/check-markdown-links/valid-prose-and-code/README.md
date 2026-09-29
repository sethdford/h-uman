# The same link in prose and in code

Prose link to a real file: [the checker](../../../../scripts/check_markdown_relative_links.py).

HTML link to a real file: <a href="../../../../scripts/check-docs-relative-links.sh">wrapper</a>.

The same text inside a code span must be ignored, not double-counted:
`[the checker](../../../../scripts/check_markdown_relative_links.py)`

```markdown
[the checker](../../../../scripts/check_markdown_relative_links.py)
<a href="../../../../scripts/check-docs-relative-links.sh">wrapper</a>
```

Expected: exit 0.
