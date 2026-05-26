# h-uman stickers

Drop image files here. h-uman will sometimes send them as reactions on
iMessage instead of typing a text reply.

## Filename schema

```
<context>-<mood>-<tone>_<seq>.<ext>
```

| context  | mood            | tone    | seq      | ext       |
|----------|-----------------|---------|----------|-----------|
| casual   | happy           | warm    | 001–999  | png, heic |
| formal   | acknowledgment  | dry     |          | jpg, jpeg |
| intimate | laugh           | earnest |          |           |
| playful  | support         |         |          |           |
|          | apology         |         |          |           |
|          | gratitude       |         |          |           |

**Example:** `casual-happy-warm_001.png`

Files that don't match this schema are silently skipped.

## How picking works

When h-uman decides to send a sticker, it filters this directory by
inferred context + mood + tone. It then prefers a file not recently sent
(LRU history tracked at `~/.human/state/sticker_lru.txt`).

If you have multiple variants per tag combo (e.g., `casual-happy-warm_001.png`
and `casual-happy-warm_002.png`), h-uman rotates between them. If you only
have one, it repeats occasionally.

## Full guide

See `docs/guides/imessage-stickers.md` in the h-uman source tree for the
complete guide, including setup instructions, configuration, and
troubleshooting.
