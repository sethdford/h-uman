---
title: Valid, Human Media Inspirations (music / YouTube / TikTok)
date: 2026-05-29
status: approved
authors: [seth, claude]
risk_tier: medium  # src/ behavior change; touches daemon reply path, no security/runtime/vtable change
---

# Valid, Human Media Inspirations

## Problem

The assistant proactively shares media "inspirations," but two things are broken:

1. **Invalid songs/links.** The only media-share feature today is the proactive
   *music share* in [`src/daemon.c:13663-13938`](../../../src/daemon.c). It asks an
   LLM (`gemini-3.1-flash-lite` @ temp 0.9) to free-text `ARTIST - TITLE`, then
   feeds that string straight into iTunes search with **no check that the
   returned track matches the suggestion**. Result: either a silent failure
   (nothing sent) or iTunes fuzzy-matches a *different* real song and the wrong
   song is sent with a valid-looking link.

2. **Robotic feel.** On unfurling channels (iMessage et al.) the rich-link path
   at [`src/daemon.c:13806-13816`](../../../src/daemon.c) sends a **bare URL with
   no text** — the human "casual message" the LLM generated is discarded to
   preserve the link preview. The recipient sees a naked Apple Music link with
   zero human framing. The generation prompt also carries **no persona/voice**.

"YouTube videos" and "TikTok inspirations" are not real senders today — TikTok
is only an inbound/outbound *channel*, and YouTube appears only because iMessage
auto-renders YouTube Music URLs the music feature may produce. This work adds
genuine, validity-guaranteed YouTube and TikTok inspiration sharing alongside
the fixed music share.

## Decisions (from brainstorming)

| Decision | Choice |
|---|---|
| Scope | Music **+** YouTube **+** TikTok |
| Human feel | **Two bubbles**: persona-voiced human line, then bare link |
| YouTube source | **YouTube Data API v3** key (graceful skip if absent) |
| TikTok reality | **Hashtag/sound deep-link** (`tiktok.com/tag/<kw>`) with human framing |
| Share decision | **One roll per turn**, pick medium by conversation cues, never >1 |
| On verify failure | **Silent skip** (text reply still goes out; no fallback junk) |

## Core principle

**The LLM never produces an identifier we send; it only produces a search
intent.** Every suggestion is resolved against a real source and verified before
anything reaches a channel. This is the same boundary discipline as
`~/.claude/rules/substring-classifier-pitfalls.md` and
`.claude/rules/security-predicate-extraction.md`: an untrusted string (the LLM's
song title / video topic) must pass a testable predicate before it drives an
action (sending a link).

## Architecture

```
per-turn reply path (daemon.c)
        │
        ▼
hu_conversation_should_share_inspiration()   ← ONE roll/turn
        │ yes                                   (reuses music_prob + crisis-skip + recent-share guards)
        ▼
hu_inspiration_pick_medium(incoming, history) ← MUSIC | YOUTUBE | TIKTOK | NONE
        │                                        (word-boundary cue match)
        ├─ MUSIC   → LLM "ARTIST - TITLE | line" → hu_music_search
        │              → hu_music_result_matches() ✓ → verified Apple/Spotify URL
        ├─ YOUTUBE → LLM "query | line" → hu_youtube_search (Data API v3)
        │              → real videoId → canonical https://www.youtube.com/watch?v=<id>
        └─ TIKTOK  → LLM "keyword | line" → hu_tiktok_tag_url(keyword)
                       → https://www.tiktok.com/tag/<url-encoded-kw>
        │
        ▼
hu_tool_validate_url(final_url)              ← belt-and-suspenders (all three)
        │ ok                                    fail → silent skip + debug log
        ▼
send by channel capability:
  unfurling  →  [persona-voiced human line]  ·human-pacing gap·  [bare URL → unfurls]
  non-unfurl →  single message: caption + .m4a preview + artwork (music legacy path)
```

### New files

| File | Role |
|---|---|
| `include/human/youtube.h` / `src/youtube.c` | `hu_youtube_search(alloc, api_key, query, len, &result)` — YouTube Data API v3 `search.list?part=snippet&q=&type=video&maxResults=1&key=`; parse `videoId` + `title` + `channelTitle`; build canonical `watch?v=<id>`. `HU_IS_TEST` guard on network; tests parse fixture JSON via a separate `hu_youtube_parse_search_response`. |
| `include/human/inspiration.h` / `src/inspiration.c` | `hu_inspiration_medium_t { HU_INSPIRATION_NONE, _MUSIC, _YOUTUBE, _TIKTOK }`; `hu_inspiration_pick_medium(incoming, len, history, count, youtube_available)`; `hu_tiktok_tag_url(keyword, len, out, cap)` (URL-encode, strip leading `#`, multi-word → single tag). |

### Modified files

| File | Change |
|---|---|
| `src/daemon.c` | Replace the music block (13663-13938) with the inspiration dispatch + two-bubble send. Keep taste-tracking + 3-7s pacing. |
| `src/context/conversation.c` | Generalize `hu_conversation_should_send_music` → `hu_conversation_should_share_inspiration` (keep crisis-skip + recent-share guards; broaden keyword boost to all media). Add per-medium prompt builders that inject persona voice. Keep `hu_conversation_should_send_music` as a thin wrapper if any caller needs it, else migrate callers. |
| `src/music.c` | Add pure predicate `hu_music_result_matches(suggested_artist, suggested_title, result)`. |
| `CMakeLists.txt` | Register `src/youtube.c`, `src/inspiration.c`, `tests/test_youtube.c`, `tests/test_inspiration.c` (+ `test_main.c` runner wiring). Gate-symmetry per `.claude/rules/test-source-gate-symmetry.md` — no new `HU_ENABLE_*` flag intended; YouTube uses libcurl HTTP like music and rides the same build. |
| config (no schema change) | YouTube key read via existing `hu_config_get_provider_key(config, "youtube")`. Document the key. |

## Mechanism 1 — validity (resolve-and-verify)

- **Music:** `hu_music_result_matches(suggested_artist, suggested_title, result)` —
  normalize both sides (lowercase; strip punctuation, `feat.`/`ft.`, trailing
  parentheticals like `(Remastered)`), then require **both** an artist token
  overlap AND a title token overlap above a threshold. Wrong-song fuzzy matches
  → rejected. No match → silent skip. Pure predicate, unit-testable with no
  network.
- **YouTube:** hallucination structurally impossible — LLM supplies a *query*,
  the Data API returns a real `videoId`, we construct the watch URL. Verified by
  construction. Empty/malformed API response → error → silent skip.
- **TikTok:** `tag/<url-encoded-keyword>` is a guaranteed-real discovery page;
  only URL-encoding to verify. (Honest limitation: this is a vibe/feed, not a
  specific clip — there is no keyless topic-search for a specific TikTok video.)
- **All three:** final `hu_tool_validate_url()` ([`src/tools/validation.c:330`](../../../src/tools/validation.c))
  before send.

## Mechanism 2 — human framing (two-bubble)

- **Unfurling channels** (`hu_channel_supports_link_unfurl()` true): send
  bubble 1 = persona-voiced human line; human-pacing gap (~1.5-3s); bubble 2 =
  bare URL. **Invariant preserved:** the URL bubble body is exactly the URL bytes
  (no preamble/whitespace/caption), per `tests/test_imessage_rich_link.c`.
- **Non-unfurling channels** (SMS): keep the existing single message =
  caption + `.m4a` preview + artwork. Inline URL is fine here (no unfurl to kill).
- **Persona shaping:** the generation prompt gets persona context
  (`humanization.formality`, `voice_rhythm`, a few traits/vocab) + the chosen
  medium, so the line sounds like the user and fits the medium ("this track",
  "this clip", "go down the #x rabbit hole") instead of the generic
  "this song is perfect for right now." The LLM returns
  `SEARCH_INTENT | human line`; we **stop discarding** the human line.

## Decision model

- One roll per turn via `hu_conversation_should_share_inspiration` (probability
  from persona `gif_probability * 0.3`, taste-boosted to 0.15, same as today;
  crisis-skip + recent-share guards retained, the latter generalized to "recent
  inspiration URL of any medium").
- If yes → `hu_inspiration_pick_medium` chooses by **word-boundary** cue match
  (per `~/.claude/rules/substring-classifier-pitfalls.md` — no naive substring):
  - music: song, listen(ing), playlist, album, artist, track, tune, vibe(s),
    mood, spotify, apple music, lyrics, concert, band, sing, karaoke
  - youtube: video, watch, clip, youtube, funny, tutorial, trailer
  - tiktok: tiktok, fyp, "for you", trend(ing), reel
  - default → music
- If chosen medium is YOUTUBE but no `youtube` key configured → fall back to
  music. Never sends more than one medium per turn.

## Testing

- `hu_music_result_matches` truth table: exact ✓; artist-only → reject;
  title-only → reject; `feat.`/parenthetical normalization; empty → reject.
- `hu_inspiration_pick_medium` cue routing incl. word-boundary negatives
  (e.g. a word merely *containing* "song") + default→music + youtube-unavailable
  fallback.
- `hu_youtube_parse_search_response` on fixture JSON → canonical URL;
  malformed/empty → error. No network in tests.
- `hu_tiktok_tag_url`: encoding, multi-word, leading-`#` strip, overflow.
- Persona-shaping prompt builder: includes voice when persona present; neutral
  when absent.
- **Two-bubble contract** (extend `tests/test_imessage_rich_link.c`): human line
  sent **then** URL; URL bubble body == URL bytes exactly.
- Non-unfurl channel: still single caption + media.
- Negative: verify-fail → zero sends.
- Full suite (12,800+) green, 0 ASan errors, before commit.

## Implementation slices

Three independently-testable slices, each green before the next:

1. **Validity predicate** — `hu_music_result_matches` + wire into the existing
   music block + tests. (Fixes "wrong song" immediately on the live music path.)
2. **Two-bubble framing + persona voice** — stop discarding the human line;
   persona-shaped generation; extend rich-link contract tests.
3. **YouTube + TikTok resolvers** — `src/youtube.c`, `src/inspiration.c`,
   medium picker, dispatch in daemon; tests.

## Out of scope / honest limitations

- No keyless "find a specific TikTok video by topic" — TikTok shares are
  discovery (hashtag) links by design.
- YouTube requires an operator-provisioned API key; without it, YouTube is
  silently skipped and the picker falls back to music.
- No new build flag; if any new source must be feature-gated later, follow
  `.claude/rules/test-source-gate-symmetry.md`.
