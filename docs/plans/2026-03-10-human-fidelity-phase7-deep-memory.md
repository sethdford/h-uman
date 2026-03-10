---
title: "Human Fidelity Phase 7 — Deep Memory & External Awareness"
created: 2026-03-10
status: draft
scope: memory, feeds, episodic, consolidation, external APIs, Apple integrations
phase: 7
features:
  [
    F70,
    F71,
    F72,
    F73,
    F74,
    F75,
    F76,
    F83,
    F84,
    F85,
    F86,
    F87,
    F88,
    F89,
    F90,
    F91,
    F92,
    F93,
  ]
parent: 2026-03-10-human-fidelity-design.md
---

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

# Human Fidelity Phase 7 — Deep Memory & External Awareness

Phase 7 of the Human Fidelity project. Implements a "real mind" with episodic memory, associative recall, memory consolidation, forgetting curves, source tagging, prospective memory, emotional residue, and external data ingestion from social media, photos, contacts, reminders, music, news, health, and email.

**Reference:** `docs/plans/2026-03-10-human-fidelity-design.md`

**Naming:** Use `hu_` prefix (project convention). Guards: `HU_IS_TEST`. Constants: `HU_SCREAMING_SNAKE`. SQLite: `SQLITE_STATIC` (null), never `SQLITE_TRANSIENT`. Free every allocation.

---

## Architecture Overview

**Approach:** Extend existing memory infrastructure. Add new SQLite tables, new C modules for episodic memory, consolidation engine, and feed ingestion. External APIs use OAuth2 token management, rate limiting, and `HU_IS_TEST` mock paths. Apple integrations use `hu_process_run` + `osascript`.

**Key integration points:**

- `src/memory/engines/sqlite.c` — schema, migrations for Phase 7 tables
- `src/memory/episodic.c` (new) — episode storage, retrieval, emotional arcs
- `src/memory/consolidation_engine.c` (new) — nightly/weekly/monthly consolidation
- `src/feeds/processor.c` (new) — background feed polling daemon
- `src/feeds/social.c` (new) — Facebook/Instagram Graph API
- `src/feeds/apple.c` (new) — Apple Photos, Contacts, Reminders, Health
- `src/feeds/google.c` (new) — Google Photos API
- `src/feeds/music.c` (new) — Spotify API
- `src/feeds/news.c` (new) — RSS/HTTP fetch
- `src/feeds/email.c` (new) — IMAP or Apple Mail
- `src/daemon.c` — proactive cycle: consolidation schedule, feed processor, prospective memory checks
- `src/agent/agent_turn.c` — inject episodic context, emotional residue, associative recall, feed items

---

## File Map

| File                                          | Responsibility                                                                    |
| --------------------------------------------- | --------------------------------------------------------------------------------- |
| `src/memory/engines/sqlite.c`                 | Schema (episodes, prospective_memories, emotional_residue, feed_items), migration |
| `src/memory/episodic.c`                       | F70 episodic storage/retrieval, F71 associative recall, F74 source tagging        |
| `include/human/memory/episodic.h`             | API, structs                                                                      |
| `src/memory/consolidation_engine.c`           | F72 nightly/weekly/monthly consolidation                                          |
| `include/human/memory/consolidation_engine.h` | API                                                                               |
| `src/memory/forgetting.c`                     | F73 salience decay, emotional anchors                                             |
| `include/human/memory/forgetting.h`           | API                                                                               |
| `src/memory/prospective.c`                    | F75 prospective memory (triggers, actions)                                        |
| `include/human/memory/prospective.h`          | API                                                                               |
| `src/memory/emotional_residue.c`              | F76 emotional weight, decay                                                       |
| `include/human/memory/emotional_residue.h`    | API                                                                               |
| `src/feeds/processor.c`                       | F93 feed processor daemon, polling                                                |
| `include/human/feeds/processor.h`             | API                                                                               |
| `src/feeds/social.c`                          | F83–F84 Facebook/Instagram OAuth2 + Graph API                                     |
| `include/human/feeds/social.h`                | API                                                                               |
| `src/feeds/apple.c`                           | F85, F87, F88, F91 Photos/Contacts/Reminders/Health                               |
| `include/human/feeds/apple.h`                 | API                                                                               |
| `src/feeds/google.c`                          | F86 Google Photos                                                                 |
| `include/human/feeds/google.h`                | API                                                                               |
| `src/feeds/music.c`                           | F89 Spotify                                                                       |
| `include/human/feeds/music.h`                 | API                                                                               |
| `src/feeds/news.c`                            | F90 RSS/HTTP                                                                      |
| `include/human/feeds/news.h`                  | API                                                                               |
| `src/feeds/email.c`                           | F92 IMAP/Apple Mail                                                               |
| `include/human/feeds/email.h`                 | API                                                                               |
| `scripts/photos_query.applescript`            | Apple Photos shared albums                                                        |
| `scripts/contacts_query.applescript`          | Apple Contacts                                                                    |
| `scripts/reminders_query.applescript`         | Apple Reminders                                                                   |
| `tests/test_episodic.c`                       | Episodic memory tests                                                             |
| `tests/test_consolidation_engine.c`           | Consolidation tests                                                               |
| `tests/test_feeds.c`                          | Feed processor + mocks                                                            |

---

## Task 1: SQLite schema — Phase 7 tables

**Description:** Add new tables to the SQLite memory backend. Tables: `episodes`, `prospective_memories`, `emotional_residue`, `feed_items`. Add `source` column to existing memory schema if not present. Schema runs on DB open.

**Files:**

- Modify: `src/memory/engines/sqlite.c`

**Steps:**

1. Extend `schema_parts[]` with:

```sql
CREATE TABLE IF NOT EXISTS episodes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL,
    summary TEXT NOT NULL,
    emotional_arc TEXT,
    key_moments TEXT,
    impact_score REAL DEFAULT 0.5,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_episodes_contact ON episodes(contact_id);
CREATE INDEX IF NOT EXISTS idx_episodes_created ON episodes(created_at);

CREATE TABLE IF NOT EXISTS prospective_memories (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    trigger_type TEXT NOT NULL,
    trigger_value TEXT NOT NULL,
    action TEXT NOT NULL,
    contact_id TEXT,
    expires_at INTEGER,
    fired INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_prospective_trigger ON prospective_memories(trigger_type, trigger_value) WHERE fired=0;
CREATE INDEX IF NOT EXISTS idx_prospective_expires ON prospective_memories(expires_at) WHERE fired=0;

CREATE TABLE IF NOT EXISTS emotional_residue (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    episode_id INTEGER,
    contact_id TEXT NOT NULL,
    valence REAL NOT NULL,
    intensity REAL NOT NULL,
    decay_rate REAL DEFAULT 0.1,
    created_at INTEGER NOT NULL,
    FOREIGN KEY (episode_id) REFERENCES episodes(id)
);
CREATE INDEX IF NOT EXISTS idx_emotional_residue_contact ON emotional_residue(contact_id);

CREATE TABLE IF NOT EXISTS feed_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source TEXT NOT NULL,
    contact_id TEXT,
    content_type TEXT NOT NULL,
    content TEXT NOT NULL,
    url TEXT,
    ingested_at INTEGER NOT NULL,
    referenced INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_feed_items_source ON feed_items(source);
CREATE INDEX IF NOT EXISTS idx_feed_items_contact ON feed_items(contact_id);
CREATE INDEX IF NOT EXISTS idx_feed_items_ingested ON feed_items(ingested_at);
```

2. Add migration logic: if `episodes` table does not exist, run CREATE. Same for other tables.

3. Ensure `memories` table has `source` column (for F74). If missing, add migration:

```sql
ALTER TABLE memories ADD COLUMN source TEXT;
```

**Tests:**

- `tests/test_episodic.c`: Insert/select episodes, prospective_memories, emotional_residue, feed_items. Verify indices. `HU_IS_TEST` path uses in-memory SQLite.

**Validation:** `./build/human_tests` — new tests pass.

---

## Task 2: Episodic memory module (F70, F71, F74)

**Description:** Implement episodic memory storage and retrieval. Full conversation episodes with emotional arcs, key moments, impact score. Associative recall via embedding-based thematic connections. Source tagging (conversation, social_media, third_party, calendar, inferred).

**Files:**

- Create: `src/memory/episodic.c`
- Create: `include/human/memory/episodic.h`

**Steps:**

1. **Structs in episodic.h:**

```c
typedef struct hu_episode {
    int64_t id;
    const char *contact_id;
    size_t contact_id_len;
    const char *summary;
    size_t summary_len;
    const char *emotional_arc;
    size_t emotional_arc_len;
    const char *key_moments;  /* JSON array */
    size_t key_moments_len;
    double impact_score;
    int64_t created_at;
    const char *source;       /* F74: conversation, social_media, third_party, calendar, inferred */
    size_t source_len;
} hu_episode_t;

typedef struct hu_episode_store {
    void *ctx;
    hu_allocator_t *alloc;
    sqlite3 *db;  /* or hu_memory_t for abstraction */
} hu_episode_store_t;
```

2. **API:**

```c
hu_episode_store_t hu_episode_store_create(hu_allocator_t *alloc, sqlite3 *db);
void hu_episode_store_deinit(hu_episode_store_t *store);

hu_error_t hu_episode_store_insert(hu_episode_store_t *store,
    const char *contact_id, size_t contact_id_len,
    const char *summary, size_t summary_len,
    const char *emotional_arc, size_t emotional_arc_len,
    const char *key_moments, size_t key_moments_len,
    double impact_score,
    const char *source, size_t source_len,
    int64_t *out_id);

hu_error_t hu_episode_store_get_by_contact(hu_episode_store_t *store,
    hu_allocator_t *alloc, const char *contact_id, size_t contact_id_len,
    size_t limit, int64_t since,
    hu_episode_t **out, size_t *out_count);

hu_error_t hu_episode_store_associative_recall(hu_episode_store_t *store,
    hu_allocator_t *alloc, hu_embedding_provider_t *embedder,
    const char *query, size_t query_len,
    const char *contact_id, size_t contact_id_len,
    size_t limit,
    hu_episode_t **out, size_t *out_count);
```

3. **Associative recall (F71):** If embedding provider available, embed query and episodes (summary + key_moments), compute cosine similarity, return top-k. Fallback: FTS5 on summary/key_moments if no embedder.

4. **Source tagging (F74):** Store `source` on insert. Valid values: `"conversation"`, `"social_media"`, `"third_party"`, `"calendar"`, `"inferred"`.

5. **Free:** Caller frees `hu_episode_t` arrays via `hu_episode_free_fields(alloc, episodes, count)`.

**Tests:**

- Insert episode, retrieve by contact, verify fields.
- Associative recall: mock embedder returns fixed vectors; verify ordering.
- `HU_IS_TEST`: use `:memory:` SQLite, no real embedder.

**Validation:** `./build/human_tests`

---

## Task 3: Forgetting curve (F73)

**Description:** Salience score decay with reinforcement. Emotional anchors resist decay.

**Files:**

- Create: `src/memory/forgetting.c`
- Create: `include/human/memory/forgetting.h`

**Steps:**

1. **Decay model:** `salience(t) = salience_0 * exp(-decay_rate * days)`. Reinforcement (re-reference) resets or boosts salience.

2. **Emotional anchors:** Episodes with `impact_score > 0.8` or tagged `emotional_anchor` use `decay_rate *= 0.3`.

3. **API:**

```c
double hu_forgetting_decayed_salience(double initial_salience, double decay_rate,
    int64_t created_at, int64_t now_ts, bool is_emotional_anchor);

void hu_forgetting_apply_decay(hu_allocator_t *alloc, sqlite3 *db,
    int64_t now_ts, double default_decay_rate);
```

4. **Integration:** `hu_forgetting_apply_decay` can update a `salience_score` column on episodes (add if not present) or compute on read. Design choice: add `salience_score` and `last_reinforced_at` to episodes table.

5. **Schema addition (if needed):**

```sql
ALTER TABLE episodes ADD COLUMN salience_score REAL DEFAULT 0.5;
ALTER TABLE episodes ADD COLUMN last_reinforced_at INTEGER;
```

**Tests:**

- Unit test decay formula: 30 days, rate 0.1 → expected value.
- Emotional anchor: decay 50% slower.

**Validation:** `./build/human_tests`

---

## Task 4: Memory consolidation engine (F72)

**Description:** Nightly/weekly/monthly offline processing of raw data → understanding. Extends `hu_memory_consolidate` with tiered schedule.

**Files:**

- Create: `src/memory/consolidation_engine.c`
- Create: `include/human/memory/consolidation_engine.h`

**Steps:**

1. **Tiers:**

- **Nightly:** Deduplicate recent memories, merge similar facts, update salience.
- **Weekly:** Summarize conversation clusters into episodes, extract themes.
- **Monthly:** Prune low-salience items, consolidate episodes into higher-level insights.

2. **API:**

```c
typedef struct hu_consolidation_engine {
    hu_allocator_t *alloc;
    hu_memory_t *memory;
    sqlite3 *db;
    hu_provider_t *provider;  /* optional, for LLM summarization */
    const char *model;
    size_t model_len;
} hu_consolidation_engine_t;

hu_error_t hu_consolidation_engine_nightly(hu_consolidation_engine_t *engine);
hu_error_t hu_consolidation_engine_weekly(hu_consolidation_engine_t *engine);
hu_error_t hu_consolidation_engine_monthly(hu_consolidation_engine_t *engine);

hu_error_t hu_consolidation_engine_run_scheduled(hu_consolidation_engine_t *engine,
    int64_t now_ts, int64_t last_run_ts);
```

3. **Nightly:** Call `hu_memory_consolidate` with config (decay_days=7, dedup_threshold=85). Update episode salience via forgetting curve.

4. **Weekly:** Query recent messages/conversations, group by contact, call provider to generate episode summaries. Insert into `episodes`.

5. **Monthly:** Delete episodes with salience < 0.1 and older than 90 days. Merge overlapping themes.

6. **`HU_IS_TEST`:** Skip LLM calls, use mock summaries. No real provider.

**Tests:**

- Nightly: run on fixture DB, verify dedup.
- Weekly: mock provider returns fixed summary, verify episode inserted.
- Monthly: verify low-salience pruning.

**Validation:** `./build/human_tests`

---

## Task 5: Prospective memory (F75)

**Description:** Future intentions — "next time she mentions X, ask about Y". Trigger types: topic, keyword, contact, time.

**Files:**

- Create: `src/memory/prospective.c`
- Create: `include/human/memory/prospective.h`

**Steps:**

1. **API:**

```c
hu_error_t hu_prospective_store(sqlite3 *db,
    const char *trigger_type, size_t trigger_type_len,
    const char *trigger_value, size_t trigger_value_len,
    const char *action, size_t action_len,
    const char *contact_id, size_t contact_id_len,
    int64_t expires_at,
    int64_t *out_id);

hu_error_t hu_prospective_check_triggers(hu_allocator_t *alloc, sqlite3 *db,
    const char *trigger_type, const char *trigger_value, size_t trigger_value_len,
    const char *contact_id, size_t contact_id_len,
    int64_t now_ts,
    hu_prospective_entry_t **out, size_t *out_count);

hu_error_t hu_prospective_mark_fired(sqlite3 *db, int64_t id);
```

2. **Trigger types:** `topic`, `keyword`, `contact`, `time`.

3. **Integration:** In agent turn, before generating response, call `hu_prospective_check_triggers` with current message topic/keywords and contact_id. Inject matched actions into prompt.

4. **Expiry:** If `expires_at > 0` and `now_ts > expires_at`, exclude from results.

**Tests:**

- Store prospective, check with matching trigger, verify returned. Mark fired, verify not returned again.
- Expired entries excluded.

**Validation:** `./build/human_tests`

---

## Task 6: Emotional residue (F76)

**Description:** Past interactions leave emotional weight that lingers and affects tone.

**Files:**

- Create: `src/memory/emotional_residue.c`
- Create: `include/human/memory/emotional_residue.h`

**Steps:**

1. **API:**

```c
hu_error_t hu_emotional_residue_add(sqlite3 *db,
    int64_t episode_id,
    const char *contact_id, size_t contact_id_len,
    double valence, double intensity,
    double decay_rate,
    int64_t *out_id);

hu_error_t hu_emotional_residue_get_active(hu_allocator_t *alloc, sqlite3 *db,
    const char *contact_id, size_t contact_id_len,
    int64_t now_ts,
    hu_emotional_residue_t **out, size_t *out_count);

void hu_emotional_residue_decay(sqlite3 *db, int64_t now_ts);
```

2. **Decay:** `intensity_current = intensity_initial * exp(-decay_rate * days)`. When intensity < 0.05, mark inactive or delete.

3. **Integration:** After episode creation, call `hu_emotional_residue_add` with valence (-1..1) and intensity (0..1). In agent turn, call `hu_emotional_residue_get_active` and inject: `[EMOTIONAL RESIDUE: Recent interaction left positive weight. Match warmth.]`

**Tests:**

- Add residue, get active, verify. Run decay, verify intensity reduced.

**Validation:** `./build/human_tests`

---

## Task 7: OAuth2 token management (shared)

**Description:** Shared OAuth2 token storage and refresh for Facebook, Instagram, Google Photos, Spotify.

**Files:**

- Create: `src/feeds/oauth.c`
- Create: `include/human/feeds/oauth.h`

**Steps:**

1. **Storage:** SQLite table `oauth_tokens`:

```sql
CREATE TABLE IF NOT EXISTS oauth_tokens (
    provider TEXT PRIMARY KEY,
    access_token TEXT NOT NULL,
    refresh_token TEXT,
    expires_at INTEGER,
    scope TEXT
);
```

2. **API:**

```c
hu_error_t hu_oauth_get_token(hu_allocator_t *alloc, sqlite3 *db,
    const char *provider, char **access_token, size_t *len);

hu_error_t hu_oauth_refresh_token(hu_allocator_t *alloc, sqlite3 *db,
    const char *provider, const char *client_id, const char *client_secret,
    const char *refresh_token);
```

3. **Refresh logic:** If `expires_at - 300 < now`, refresh before use. Store new tokens.

4. **`HU_IS_TEST`:** Return mock token, never hit network.

**Tests:**

- Store token, retrieve. Mock refresh.

**Validation:** `./build/human_tests`

---

## Task 8: Feed processor daemon (F93)

**Description:** Background polling service that invokes feed ingestors on a schedule.

**Files:**

- Create: `src/feeds/processor.c`
- Create: `include/human/feeds/processor.h`

**Steps:**

1. **API:**

```c
typedef struct hu_feed_processor {
    hu_allocator_t *alloc;
    sqlite3 *db;
    hu_memory_t *memory;
    const char *config_path;
} hu_feed_processor_t;

hu_error_t hu_feed_processor_poll(hu_feed_processor_t *proc, int64_t now_ts);
```

2. **Poll schedule:** Configurable per source (e.g. Facebook: 15 min, RSS: 30 min, Photos: hourly). Read from `~/.human/feeds.json` or persona config.

3. **Flow:** For each enabled source, if `now_ts - last_poll[source] >= interval`, call source-specific ingestor, insert into `feed_items`, update last_poll.

4. **Integration:** Daemon proactive cycle calls `hu_feed_processor_poll` every N minutes.

5. **`HU_IS_TEST`:** Skip real network, use fixture feed_items.

**Tests:**

- Mock config, run poll, verify feed_items inserted (from mock ingestor).

**Validation:** `./build/human_tests`

---

## Task 9: Facebook feed ingestion (F83)

**Description:** Facebook Graph API feed ingestion. OAuth2, rate limiting, error handling.

**Files:**

- Create: `src/feeds/social.c` (Facebook + Instagram)
- Create: `include/human/feeds/social.h`

**Steps:**

1. **OAuth2:** Use `hu_oauth_get_token` for `"facebook"`. Refresh if needed.

2. **API call:** `GET https://graph.facebook.com/v18.0/me/feed?fields=id,message,created_time,permalink_url&access_token=TOKEN&limit=25`

3. **Rate limit:** Facebook allows ~200 calls/hour. Track last call time, sleep if needed. Config: `facebook_rate_limit_ms`.

4. **Error handling:** 401 → refresh token, retry once. 429 → backoff. 4xx/5xx → log, skip this cycle.

5. **Insert:** For each post, insert into `feed_items` with `source='facebook'`, `content_type='post'`, `content=message`, `url=permalink_url`.

6. **`HU_IS_TEST`:** Return mock JSON, no real HTTP.

**Tests:**

- Parse mock Graph API response, verify feed_items structure.

**Validation:** `./build/human_tests`

---

## Task 10: Instagram feed ingestion (F84)

**Description:** Instagram Basic Display API. OAuth2.

**Files:**

- Extend: `src/feeds/social.c`
- Extend: `include/human/feeds/social.h`

**Steps:**

1. **OAuth2:** Provider `"instagram"`.

2. **API:** `GET https://graph.instagram.com/v18.0/me/media?fields=id,caption,media_type,media_url,timestamp&access_token=TOKEN`

3. **Rate limit:** 200 calls/hour. Same pattern as Facebook.

4. **Insert:** `source='instagram'`, `content_type='media'`, `content=caption`.

5. **`HU_IS_TEST`:** Mock response.

**Tests:**

- Parse mock Instagram response.

**Validation:** `./build/human_tests`

---

## Task 11: Apple Photos shared albums (F85)

**Description:** AppleScript → Photos.app for shared albums.

**Files:**

- Create: `scripts/photos_query.applescript`
- Create: `src/feeds/apple.c`
- Create: `include/human/feeds/apple.h`

**Steps:**

1. **AppleScript:**

```applescript
-- photos_query.applescript
-- Usage: osascript photos_query.applescript [album_name]
tell application "Photos"
    set sharedAlbums to every shared album
    set output to ""
    repeat with a in sharedAlbums
        set aname to name of a
        set output to output & aname & "|"
        set mediaItems to every media item of a
        repeat with m in mediaItems
            set output to output & (filename of m) & ";" & (date of m as text) & "|"
        end repeat
        set output to output & "---"
    end repeat
    return output
end tell
```

2. **C wrapper:**

```c
hu_error_t hu_apple_photos_get_shared_albums(hu_allocator_t *alloc,
    char **out_json, size_t *out_len);
```

- `hu_process_run(alloc, {"osascript", "-e", script_or_path, NULL}, ...)`
- Parse stdout (pipe-delimited or JSON), convert to `feed_items` format.
- `source='apple_photos'`, `content_type='photo'`.

3. **`HU_IS_TEST`:** Skip `osascript`, return fixture string.

**Tests:**

- Parse fixture output, verify feed_items.

**Validation:** `./build/human_tests`

---

## Task 12: Google Photos shared albums (F86)

**Description:** Google Photos API, OAuth2.

**Files:**

- Create: `src/feeds/google.c`
- Create: `include/human/feeds/google.h`

**Steps:**

1. **OAuth2:** Provider `"google_photos"`. Scope: `https://www.googleapis.com/auth/photoslibrary.readonly`.

2. **API:** `GET https://photoslibrary.googleapis.com/v1/albums` then `POST https://photoslibrary.googleapis.com/v1/mediaItems:search` for shared albums.

3. **Rate limit:** 10,000 requests/day. Throttle to ~1 req/sec if needed.

4. **Insert:** `source='google_photos'`, `content_type='photo'`.

5. **`HU_IS_TEST`:** Mock response.

**Tests:**

- Parse mock Google Photos response.

**Validation:** `./build/human_tests`

---

## Task 13: Apple Contacts sync (F87)

**Description:** AppleScript → Contacts.app.

**Files:**

- Create: `scripts/contacts_query.applescript`
- Extend: `src/feeds/apple.c`

**Steps:**

1. **AppleScript:**

```applescript
tell application "Contacts"
    set people to every person
    set output to "["
    repeat with p in people
        set output to output & "{\"name\":\"" & (name of p as text) & "\",\"phones\":["
        set phones to phones of p
        repeat with ph in phones
            set output to output & "\"" & (value of ph as text) & "\","
        end repeat
        set output to output & "]}"
    end repeat
    set output to output & "]"
    return output
end tell
```

2. **C:** `hu_apple_contacts_get_all(alloc, &json, &len)`. Parse, store in memory or contact profiles. Not necessarily `feed_items` — may be separate contact sync table.

3. **`HU_IS_TEST`:** Return fixture JSON.

**Tests:**

- Parse fixture, verify structure.

**Validation:** `./build/human_tests`

---

## Task 14: Apple Reminders awareness (F88)

**Description:** AppleScript → Reminders.app.

**Files:**

- Create: `scripts/reminders_query.applescript`
- Extend: `src/feeds/apple.c`

**Steps:**

1. **AppleScript:**

```applescript
tell application "Reminders"
    set lists to every list
    set output to ""
    repeat with l in lists
        set items to (every reminder of l whose completed is false)
        repeat with r in items
            set output to output & (name of r as text) & "|" & (due date of r as text) & "|"
        end repeat
    end repeat
    return output
end tell
```

2. **C:** `hu_apple_reminders_get_incomplete(alloc, &json, &len)`.

3. **Integration:** Inject into context for "what's on my plate" awareness.

4. **`HU_IS_TEST`:** Fixture.

**Tests:**

- Parse fixture.

**Validation:** `./build/human_tests`

---

## Task 15: Spotify/Apple Music awareness (F89)

**Description:** Spotify Web API for recently played.

**Files:**

- Create: `src/feeds/music.c`
- Create: `include/human/feeds/music.h`

**Steps:**

1. **OAuth2:** Provider `"spotify"`. Scope: `user-read-recently-played`.

2. **API:** `GET https://api.spotify.com/v1/me/player/recently-played?limit=20` with `Authorization: Bearer TOKEN`.

3. **Rate limit:** 180 requests/min. Throttle.

4. **Insert:** `source='spotify'`, `content_type='track'`, `content=track name + artist`.

5. **Apple Music:** No public API for recently played without MusicKit. Document as future work or skip.

6. **`HU_IS_TEST`:** Mock.

**Tests:**

- Parse mock Spotify response.

**Validation:** `./build/human_tests`

---

## Task 16: News/RSS topical awareness (F90)

**Description:** HTTP fetch of RSS/Atom feeds.

**Files:**

- Create: `src/feeds/news.c`
- Create: `include/human/feeds/news.h`

**Steps:**

1. **HTTP:** Use `hu_http_get` (libcurl). Config: list of feed URLs in `feeds.json`.

2. **Parse:** Simple XML parse for `<item><title>`, `<link>`, `<description>`, `<pubDate>`. Or use minimal regex for common RSS structure.

3. **Rate limit:** 1 request per feed per 30 min default.

4. **Insert:** `source='rss'` or feed name, `content_type='article'`, `content=title + description`.

5. **`HU_IS_TEST`:** `hu_http_get` already mocks in `HU_IS_TEST` (see `http.c`).

**Tests:**

- Parse fixture RSS XML.

**Validation:** `./build/human_tests`

---

## Task 17: Apple Health awareness (F91)

**Description:** HealthKit export XML parsing. User exports from Health app, we parse.

**Files:**

- Extend: `src/feeds/apple.c`

**Steps:**

1. **Input:** Path to `export.xml` (Health app export). Config: `health_export_path` in persona or feeds config.

2. **Parse:** Minimal XML parser for `<Record type="HKQuantityTypeIdentifier...">` with `startDate`, `value`, `unit`. Focus on steps, heart rate, sleep if available.

3. **Store:** Summary stats (e.g. steps today, sleep last night) in memory or feed_items with `source='apple_health'`.

4. **`HU_IS_TEST`:** Parse fixture XML, no file I/O.

**Tests:**

- Parse fixture Health export XML.

**Validation:** `./build/human_tests`

---

## Task 18: Email awareness (F92)

**Description:** IMAP or Apple Mail AppleScript.

**Files:**

- Create: `src/feeds/email.c`
- Create: `include/human/feeds/email.h`

**Steps:**

1. **IMAP:** Use libcurl IMAP or external `curl imap://...`. Fetch recent headers (subject, from, date). Config: `imap_server`, `username`, `password` (or OAuth2 for Gmail).

2. **Apple Mail:** AppleScript:

```applescript
tell application "Mail"
    set inbox to inbox
    set messages to (every message of inbox whose read status is false)
    set output to ""
    repeat with m in messages
        set output to output & (subject of m as text) & "|" & (sender of m as text) & "|"
    end repeat
    return output
end tell
```

3. **Insert:** `source='email'`, `content_type='email'`, `content=subject + from`.

4. **`HU_IS_TEST`:** Mock. No real IMAP or Mail.

**Tests:**

- Parse fixture email list.

**Validation:** `./build/human_tests`

---

## Task 19: CMake integration

**Description:** Add `HU_ENABLE_SOCIAL` and `HU_ENABLE_FEEDS` CMake options. Gate social/feed modules.

**Files:**

- Modify: `CMakeLists.txt`
- Modify: `src/memory/CMakeLists.txt` or equivalent
- Create: `src/feeds/CMakeLists.txt`

**Steps:**

1. Add options:

```cmake
option(HU_ENABLE_SOCIAL "Build Facebook/Instagram feed ingestion" OFF)
option(HU_ENABLE_FEEDS "Build feed processor and external data ingestion" OFF)
```

2. When `HU_ENABLE_FEEDS=ON`, add `src/feeds/processor.c`, `src/feeds/news.c`, `src/feeds/email.c`, `src/feeds/apple.c` (Apple-only). Link `hu_http_*` when `HU_ENABLE_CURL=ON`.

3. When `HU_ENABLE_SOCIAL=ON`, add `src/feeds/social.c`, `src/feeds/oauth.c`. Requires `HU_ENABLE_FEEDS=ON`.

4. Add `src/feeds/google.c` when `HU_ENABLE_FEEDS=ON` (Google Photos).
5. Add `src/feeds/music.c` when `HU_ENABLE_FEEDS=ON` (Spotify).

6. Episodic, consolidation_engine, forgetting, prospective, emotional_residue: add unconditionally (or behind `HU_ENABLE_PERSONA` / `HU_ENABLE_SKILLS` if that's the gate for advanced memory).

**Validation:** `cmake -B build -DHU_ENABLE_FEEDS=ON -DHU_ENABLE_SOCIAL=ON && cmake --build build`

---

## Task 20: Daemon and agent integration

**Description:** Wire consolidation schedule, feed processor, prospective memory, emotional residue, episodic context into daemon and agent turn.

**Files:**

- Modify: `src/daemon.c`
- Modify: `src/agent/agent_turn.c` or `src/agent/memory_loader.c`

**Steps:**

1. **Daemon:** In proactive cycle:
   - Every 24h: `hu_consolidation_engine_run_scheduled(engine, now, last_run)`.
   - Every 15 min (or configurable): `hu_feed_processor_poll(proc, now)`.

2. **Agent turn:** Before generating response:
   - `hu_prospective_check_triggers(..., message_topic, contact_id, ...)` → inject actions.
   - `hu_emotional_residue_get_active(..., contact_id, ...)` → inject residue context.
   - `hu_episode_store_get_by_contact` or `hu_episode_store_associative_recall` → inject episodic context.
   - `hu_feed_items_get_recent(..., contact_id, limit)` → inject feed items if relevant.

3. **After response:** If new conversation ended, create episode via `hu_episode_store_insert`, add emotional residue if applicable.

**Tests:**

- Integration test: mock daemon cycle, verify consolidation and feed poll called.
- Agent: mock memory loader returns episodic + residue, verify prompt contains them.

**Validation:** `./build/human_tests`

---

## Validation Matrix

| Check           | Command                                                                            |
| --------------- | ---------------------------------------------------------------------------------- |
| Build (minimal) | `cmake -B build && cmake --build build`                                            |
| Build (feeds)   | `cmake -B build -DHU_ENABLE_FEEDS=ON -DHU_ENABLE_SOCIAL=ON && cmake --build build` |
| Tests           | `./build/human_tests`                                                              |
| ASan            | `cmake -B build -DHU_ENABLE_ASAN=ON && cmake --build build && ./build/human_tests` |
| Release         | `cmake -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON`                  |

---

## Config Schema (feeds.json)

```json
{
  "facebook": {
    "enabled": false,
    "poll_interval_min": 15
  },
  "instagram": {
    "enabled": false,
    "poll_interval_min": 15
  },
  "apple_photos": {
    "enabled": false,
    "poll_interval_min": 60
  },
  "google_photos": {
    "enabled": false,
    "poll_interval_min": 60
  },
  "spotify": {
    "enabled": false,
    "poll_interval_min": 30
  },
  "rss": {
    "enabled": false,
    "urls": [],
    "poll_interval_min": 30
  },
  "email": {
    "enabled": false,
    "provider": "imap",
    "poll_interval_min": 15
  },
  "apple_health": {
    "enabled": false,
    "export_path": null
  }
}
```

---

## Risk Notes

| Risk                           | Mitigation                                                      |
| ------------------------------ | --------------------------------------------------------------- |
| OAuth tokens in config         | Store in SQLite with restricted permissions; never log tokens   |
| AppleScript requires app focus | Run in background; Photos/Contacts/Reminders don't need focus   |
| Rate limit violations          | Conservative defaults; exponential backoff on 429               |
| Binary size                    | All feed modules gated by CMake flags; default OFF              |
| Cross-contact privacy          | Feed items with contact_id only shown to that contact's context |
