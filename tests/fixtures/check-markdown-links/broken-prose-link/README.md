# One genuinely broken link in prose

The fenced block is noise the checker must ignore:

```
[fenced](this-one-is-code.md)
```

The reference definition below follows the fence directly. It is real prose,
its target does not exist, and the checker must report it and exit 1.

[gone]: ./does-not-exist.md
