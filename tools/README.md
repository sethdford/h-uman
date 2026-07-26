# tools/ — ad-hoc measurement instruments

One-shot programs used to answer a specific measurement question. **Not part of
the CMake build** — each is compiled by hand against `build/libhuman_core.a`
when needed, then discarded until the next time that question comes up.

They live here rather than at the repo root (where they accumulated as untracked
scratch) so they survive worktree cleanup and are findable by the next session.

| File | Question it answers | Related work |
|---|---|---|
| `dump_prompt.c` | What is the *byte-faithful* persona system prompt the send path produces? Has a `faithful` mode replicating the production `tone_note` block. | blind-A/B prompt fidelity |
| `extract_yield.c` | Does the LLM extractor (`hu_fact_extract_llm`) recover meaningfully more than the dict scanner (`hu_fast_capture`) on the same corpus? | starved-subsystems finding; decides whether wiring the LLM path is worth it |
| `tom_backfill.c` | Backfills ToM beliefs into a **separate** DB by replaying the daemon's live belief-recording path over historical inbound messages. | ToM SHADOW A/B |

## Building

Each file's header comment carries its usage string. They link against
`build/libhuman_core.a` from a dev/ASan build, but the exact link line is **not
captured here** — a naive
`cc -std=c11 -fsanitize=address -Iinclude -Ibuild <file> build/libhuman_core.a -lsqlite3 -lcurl -lm`
still leaves symbols unresolved. If you get one linking, record the working
command in this table so the next person doesn't repeat the search.

## Safety

`tom_backfill.c` writes to a backfill DB passed as `argv` and must **never** be
pointed at the live `~/.human/memory.db`. `dump_prompt.c` and `extract_yield.c`
are read-only with respect to production state, but `extract_yield.c` issues
real requests to whatever `base_url` you give it — point it at a spare server
(`:8743`), not the live `:8741`.
