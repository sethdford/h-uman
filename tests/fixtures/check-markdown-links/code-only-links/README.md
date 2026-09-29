# Link shapes that live only inside code

Every `[text](target)` shape in this file is inside an inline code span or a
fenced block, so the checker must report nothing and exit 0.

The regex from the 2026-09-03 incident (commit 30568c04d, CI run 33732740550):

| pattern | note |
|---|---|
| `\w+['’](t\|re\|ve\|ll\|d\|s\|m)` | contraction suffixes |

A double-backtick span whose content is a single-backtick span: `` `[not](a-link.md)` ``.

~~~
[tilde fence](missing-tilde.md)
~~~

   ```python
   href = "[indented fence](missing-indented.md)"
   ```

```
[closing fence longer than opening](missing-longer-close.md)
````

<!-- the `[x](y)` above closed with four backticks, which CommonMark allows -->
