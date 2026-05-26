# iMessage Stickers

## Overview

When h-uman decides to send a casual visual acknowledgment on iMessage instead of typing a text reply, it can pick a sticker from your `~/.human/stickers/` directory. This happens when the persona's reply-style predicate chooses a `TAPBACK`-style reaction or when a brief visual acknowledgment feels more natural than words.

Stickers are typically emojis, reaction images, or other visual responses — think a thumbs-up GIF, a hand-wave, or a subtle laugh reaction.

## Honest Caveat: Attachment, Not Native Sticker Balloon

h-uman stickers are sent as **image attachments**, NOT as native Apple sticker balloons. Here's the difference:

- **Visual appearance**: Recipients see the sticker as a regular photo in the chat bubble, not as a special interactive sticker balloon with the blue-tinted bubble border that native iMessage stickers get.
- **Why the difference**: h-uman runs on all platforms (Linux, Mac, Windows, iOS, Android). Native `IMSticker` balloon integration would require Apple's private framework, which the company locked down starting in macOS 26+ (same wall that blocks typing indicators and read receipts in iMessage).
- **In practice**: Power users who know native stickers will notice the difference. Most recipients won't — a sticker is a sticker.

If native sticker balloons matter for your use case, you can disable stickers entirely (see Configuration below) and h-uman will fall back to text replies.

## Setup

Create the sticker directory and add images:

```bash
mkdir -p ~/.human/stickers/
cd ~/.human/stickers/
# Drop .png, .heic, .jpg, or .jpeg files here
# Use the filename schema described below
```

## Filename Schema

Each sticker file must follow this naming pattern:

```
<context>-<mood>-<tone>_<seq>.<ext>
```

### Components

| Component | Allowed values | Purpose |
|-----------|---|---|
| **context** | `casual`, `formal`, `intimate`, `playful` | Conversational setting |
| **mood** | `happy`, `acknowledgment`, `laugh`, `support`, `apology`, `gratitude` | Emotional tone |
| **tone** | `warm`, `dry`, `earnest` | Communication style |
| **seq** | `001`–`999` | Sequence number (allows multiple variants per tag combo) |
| **ext** | `png`, `heic`, `jpg`, `jpeg` | Image format |

### Examples

- `casual-happy-warm_001.png` — friendly acknowledgment for casual chat (sun emoji vibe)
- `intimate-support-earnest_001.heic` — heart-hands for moments that matter
- `playful-laugh-dry_001.jpg` — deadpan reaction, stolen from a GIF still
- `formal-acknowledgment-earnest_001.png` — clean checkmark for business contexts
- `casual-gratitude-warm_002.png` — wave and smile (the `_002` is a second variant)

### Invalid Names (Silently Skipped)

Files that don't match the schema are ignored without warning:

- `thanks.png` — no tags
- `casual-happy.png` — missing tone
- `casual-happy-warm.png` — missing `_seq`
- `casual-happy-tone_001.png` — "tone" is not a valid value
- `my-vacation-pic.jpg` — doesn't start with valid context

## How Sticker Selection Works

When h-uman decides to send a sticker:

1. **Build a query** — The persona's reply-style decision includes inferred context (is this a casual text or a formal email?) and mood (should the response be happy, supportive, etc.). It creates a query like:
   ```
   context: "casual"
   mood: "happy"
   tone: "warm"
   ```

2. **Filter the directory** — h-uman scans `~/.human/stickers/` and keeps all files whose tags EXACTLY match the query. Any field in the query can be NULL, which means "match anything for this field":
   - Query `(casual, happy, NULL)` matches `casual-happy-warm_001.png` AND `casual-happy-dry_001.png`
   - Query `(casual, NULL, NULL)` matches any file starting with `casual-`

3. **Apply LRU rotation** — To avoid sending the same sticker repeatedly, h-uman tracks recently-sent files in `~/.human/state/sticker_lru.txt`. When multiple files match the query:
   - Files NOT in the recent LRU head are preferred (so you don't repeat)
   - Among preferred files, one is chosen uniformly at random
   - The chosen file is recorded in the LRU (at the front, with a max of 100 entries)
   - The same sticker can repeat only when NO other file matches the query

4. **Send as attachment** — The picked file is sent as an image attachment in the iMessage.

## Configuration

### Default

By default, stickers are loaded from `~/.human/stickers/`. If that directory doesn't exist or is empty, h-uman falls back to sending text replies.

### Custom Location

To store stickers elsewhere, set `iMessage.action_surface_v2.sticker_dir` in your `~/.human/config.json`:

```json
{
  "iMessage": {
    "action_surface_v2": {
      "sticker_dir": "/Users/you/Dropbox/my-stickers"
    }
  }
}
```

### Disable Stickers

To disable sticker sending entirely (and always fall back to text), set the config key to a non-existent path:

```json
{
  "iMessage": {
    "action_surface_v2": {
      "sticker_dir": "/dev/null"
    }
  }
}
```

## Getting Started: Tips

- **Start small** — Begin with 5–10 stickers across 2–3 tag combinations. As you notice moments where h-uman could use a visual reaction, add more.
- **Multiple variants per combo** — If you have 3 variants of `casual-happy-warm`, h-uman rotates through them; if you only have one, it repeats occasionally. Aim for at least 2–3 files per tag combo to make LRU rotation effective.
- **File size** — Use HEIC format if you don't need cross-platform compatibility (smaller file size). PNG is best for broad compatibility.
- **Source** — Animated GIFs should be converted to still images (first frame or a key frame). h-uman doesn't send video or animated formats.

## Troubleshooting

### No stickers are ever sent

Check the log at `~/.human/logs/imessage_action.jsonl`. If you see lines with `"tier":"flat_fallback"`, the sticker picker returned false and h-uman sent text instead.

Common causes:
- Directory doesn't exist: `mkdir -p ~/.human/stickers/` and add files
- No files match the query: If the query is `(casual, happy, warm)` but you only have `casual-happy-dry_001.png`, there's no match. Either add a `casual-happy-warm` file or h-uman widens the query.
- Filenames invalid: Check that your files match the schema exactly (`casual-happy-warm_001.png`, not `casual-happy-warm_001-final.png`)

### The same sticker keeps repeating

You likely have only one file for a given tag combination. Add more variants:

```bash
# If you only have casual-happy-warm_001.png
# Add a second one:
cp casual-happy-warm_001.png casual-happy-warm_002.png
# Edit the second to be different (or keep a variant around)
```

The LRU file at `~/.human/state/sticker_lru.txt` tracks the history. You can inspect it:

```bash
cat ~/.human/state/sticker_lru.txt
```

If the LRU isn't being written, check permissions on `~/.human/state/`.

### Configuration not being picked up

If you added `sticker_dir` to `config.json` and h-uman still uses the default path:

- Confirm the JSON is valid (use `jq . ~/.human/config.json` to check)
- Confirm the path is inside the `iMessage.action_surface_v2` block (not at the top level)
- Restart the daemon: `pkill -f hu_daemon; human daemon start`

## See Also

- User-visible sticker guide template: `assets/sticker-readme-template.md` (copy to `~/.human/stickers/README.md` for a quick reference in your stickers directory)
- Full h-uman configuration: `docs/guides/config.md`
- iMessage action surface implementation: `docs/plans/*/iMessage-action-surface*`
