# src/feeds/ — Feed Processor and Sources

External data ingestion from social, messaging, news, and file sources.
Feed sources gated behind `HU_ENABLE_FEEDS`, storage behind `HU_ENABLE_SQLITE`.

## Testing

- `test_feeds.c`, `test_feed_processor.c`, `test_research_feeds.c`

## Architecture

```
processor.c           SQL generation, relevance scoring, prompt building, poll loop
gmail.c               Gmail feed source (OAuth, IMAP)
imessage.c            iMessage feed source (macOS ChatDB)
twitter.c             Twitter/X feed source (Bearer token API)
social.c              Social feed source (Facebook, Instagram, TikTok)
research.c            Research feed source
research_executor.c   Research execution engine
findings.c            Research findings storage
trends.c              Trend detection and tracking
news.c                News/RSS feed source
file_ingest.c         Local file ingestion from ~/.human/ingest/
```

## Feed Types

`hu_feed_type_t` enum covers: social (Facebook, Instagram, TikTok), photos (Apple, Google),
contacts, reminders, health, email, Gmail, iMessage, Twitter, news RSS, file ingest.

## Key Structs

- `hu_feed_item_t` — heap-allocated item with source/content/topic pointers
- `hu_feed_item_stored_t` — fixed-buffer item for SQLite storage (content: 2048 bytes)
- `hu_feed_ingest_item_t` — fixed-buffer item returned by feed sources
- `hu_feed_config_t` — per-type enabled flags and poll intervals
- `hu_feed_processor_t` — alloc + sqlite3 db handle

## HU_IS_TEST Mock Pattern

All feed sources return mock data when `HU_IS_TEST` is defined:

- Gmail: 2 mock emails
- iMessage: 2 mock messages
- Twitter: 2 mock tweets
- File ingest: 1 mock document

Real credential-gated code paths are `#ifdef`-guarded.

## Rules

- Cap `memcpy` of content to `sizeof(content) - 1` and null-terminate
- All SQL generation uses `escape_sql_string` — never interpolate raw user data
- Feed sources never make real network calls in test mode
- `hu_feed_item_deinit` must free all heap-allocated fields
- Use `hu_str_free` (not raw `free`) for string fields in `hu_feed_item_t`
