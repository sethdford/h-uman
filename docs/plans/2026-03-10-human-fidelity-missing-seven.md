---
title: "Project Human Fidelity — The Missing Seven: Cross-Cutting Systems for Superhuman Fidelity"
created: 2026-03-10
status: proposed
scope: Visual content pipeline, proactive governor, knowledge state, collaborative planning, context arbitration, relationship dynamics, shared compression
parent: 2026-03-10-human-fidelity-design.md
new_features: F116–F143 (28 features)
new_pillars: 7 (Pillars 18–24)
---

# The Missing Seven — Cross-Cutting Systems for Superhuman Fidelity

> Seven systems the original plan lacks that separate "good AI" from "indistinguishable human." These are not nice-to-haves — without them, the uncanny valley is inescapable.

## Why These Seven

The original 115 features across 9 phases cover _what_ to say and _when_ to say it. These seven systems cover the connective tissue between those features — the meta-layer that makes all 115 features work in concert instead of as independent probability rolls. Each missing system was identified by asking: "What does a real human do effortlessly that this plan can't?"

| #   | System                        | The Gap                                                    | What a Real Human Does                                                                                   |
| --- | ----------------------------- | ---------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| 1   | Visual Content Pipeline       | Plan can receive photos but never sends them               | Shares screenshots, photos, links, memes — 30%+ of iMessage content is visual                            |
| 2   | Proactive Volume Governor     | 20+ independent proactive features with no global cap      | Intuitively knows when they're texting too much; has a sense of "I've reached out enough today"          |
| 3   | Contact Knowledge State       | No model of what each contact knows about your life        | Never tells the same story twice to the same person; knows who to tell what                              |
| 4   | Collaborative Planning        | Can follow up on plans but can't initiate them             | "We should do X," "Are you free Saturday?", "I just saw tickets for Y"                                   |
| 5   | Context Arbitration           | 20+ simultaneous prompt directives with no priority system | Naturally weights what matters right now; doesn't try to be funny, empathetic, and proactive all at once |
| 6   | Relationship Dynamics         | Static relationship stages with no trajectory modeling     | Senses when a friendship is drifting, deepening, or in repair mode                                       |
| 7   | Shared Experience Compression | Inside jokes exist but no ultra-compressed shared language | "IYKYK" references, one-word callbacks, shared shorthand that only makes sense to two people             |

## Architecture Principle

All seven systems follow the existing BTH Enhancement approach:

- No new vtable interfaces
- No new dependencies
- Persona JSON configuration
- SQLite persistence
- Integration through the existing daemon proactive cycle and conversation context builder
- Each system exposes a thin C API consumed by `daemon.c` and `conversation.c`

**Key constraint**: These seven systems are interdependent. The Proactive Governor (2) gates the Visual Pipeline (1), Planning (4), and every other proactive feature. The Knowledge State (3) feeds the Context Arbitrator (5), which decides what to inject into the prompt. The Relationship Dynamics model (6) modulates the Governor's budget. The Shared Compression system (7) produces ultra-efficient context that helps the Arbitrator fit more signal into less tokens.

```
┌─────────────────────────────────────────────────────────────┐
│                    DAEMON PROACTIVE CYCLE                    │
│                                                             │
│  ┌──────────┐  ┌──────────────┐  ┌───────────────────────┐ │
│  │ Visual   │  │ Collaborative│  │ All existing proactive│ │
│  │ Pipeline │  │ Planning     │  │ features (F8,F12,F25, │ │
│  │ (1)      │  │ (4)          │  │ F30,F31,F50,F53...)   │ │
│  └────┬─────┘  └──────┬───────┘  └───────────┬───────────┘ │
│       │               │                      │              │
│       ▼               ▼                      ▼              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │           PROACTIVE VOLUME GOVERNOR (2)                │ │
│  │     Global budget × Contact budget × Relationship      │ │
│  │     dynamics (6) → priority queue → top N actions       │ │
│  └────────────────────────┬───────────────────────────────┘ │
│                           │                                 │
│                           ▼                                 │
│  ┌────────────────────────────────────────────────────────┐ │
│  │           CONTEXT ARBITRATION LAYER (5)                │ │
│  │     Knowledge State (3) + Shared Compression (7)       │ │
│  │     + Relationship Dynamics (6) → ranked directives    │ │
│  │     → token-budgeted prompt injection                  │ │
│  └────────────────────────┬───────────────────────────────┘ │
│                           │                                 │
│                           ▼                                 │
│                    LLM PROMPT + RESPONSE                    │
└─────────────────────────────────────────────────────────────┘
```

---

## Pillar 18: Visual Content Pipeline (Outbound)

> A human who never shares photos, screenshots, links, or memes is suspicious. This is not a stretch goal — it's table stakes.

### Features

| #    | Feature                                                                                  | Complexity | Phase | Status         |
| ---- | ---------------------------------------------------------------------------------------- | ---------- | ----- | -------------- |
| F116 | **Photo sharing engine** — query Apple Photos for relevant content to share              | High       | 4     | ✅ Implemented |
| F117 | **Link sharing** — share URLs with natural context framing                               | Medium     | 4     | ✅ Implemented |
| F118 | **Screenshot capture** — programmatic screenshot of web content to share                 | Medium     | 4     | ✅ Implemented |
| F119 | **Visual decision engine** — when to send visual vs text, what format                    | High       | 4     | ✅ Implemented |
| F120 | **Visual context matching** — match visual content to contact interests and conversation | High       | 4     | ✅ Implemented |

### F116: Photo Sharing Engine

The system runs on the persona owner's Mac. It has full access to their Apple Photos library. This is a massive, underutilized asset.

**Architecture:**

```
Photos.app (SQLite: ~/Pictures/Photos Library.photoslibrary/database/Photos.sqlite)
    │
    ▼
hu_visual_scan_photos() — periodic scan (every 6 hours)
    │
    ├─ Recent photos (last 7 days): extract path, date, location, faces
    ├─ Vision description: hu_vision_describe_image() for each new photo
    └─ Index into visual_content table with embeddings
    │
    ▼
hu_visual_match_for_contact() — finds shareable photos for a contact
    │
    ├─ Match by: shared interests, mentioned topics, location overlap,
    │   mutual friends (faces), seasonal relevance
    ├─ Exclude: private/sensitive content (configurable blocklist)
    └─ Return ranked candidates with relevance score
```

**Apple Photos access** (read-only, via SQLite):

```sql
-- Query recent photos from Photos.app database
SELECT
    Z_PK, ZDATECREATED, ZLATITUDE, ZLONGITUDE,
    ZDIRECTORY, ZFILENAME, ZUNIFORMTYPEIDENTIFIER
FROM ZASSET
WHERE ZDATECREATED > ?
ORDER BY ZDATECREATED DESC
LIMIT 50;
```

The photo library path is typically `~/Pictures/Photos Library.photoslibrary/database/Photos.sqlite`. We read this in `HU_IS_TEST`-guarded code — no writes, no modifications.

**Sharing triggers:**

- **Reactive**: Contact mentions a place you've been → share a photo from there
- **Proactive**: You took a great sunset photo → share with close contacts who appreciate that
- **Conversational**: "Check this out" / "look at this" as natural interjections
- **Seasonal**: Holiday photos, event photos shared at relevant times

**Privacy safeguards:**

- Never share photos containing faces of people the contact doesn't know (face-to-contact mapping required)
- Never share photos tagged as private/hidden in Photos.app
- Never share photos from sensitive locations (configurable blocklist: medical, legal, etc.)
- Rate limit: maximum 2 photo shares per contact per day

**C API:**

```c
typedef struct hu_visual_candidate {
    char *path;
    size_t path_len;
    char *description;
    size_t description_len;
    double relevance_score;
    char *sharing_context;  /* "I took this yesterday" / "reminded me of..." */
    size_t sharing_context_len;
} hu_visual_candidate_t;

hu_error_t hu_visual_scan_recent(hu_allocator_t *alloc, hu_memory_t *memory,
                                 uint64_t since_ms);

hu_error_t hu_visual_match_for_contact(hu_allocator_t *alloc, hu_memory_t *memory,
                                       const char *contact_id, size_t contact_id_len,
                                       const char *conversation_context, size_t ctx_len,
                                       hu_visual_candidate_t *out, size_t *out_count,
                                       size_t max_candidates);

void hu_visual_candidate_deinit(hu_visual_candidate_t *c, hu_allocator_t *alloc);
```

### F117: Link Sharing

Share URLs with natural framing. Sources:

- **RSS feeds** (already planned in F91): article matches contact interest → "you need to read this"
- **Browsing history**: Safari reading list (SQLite at `~/Library/Safari/Bookmarks.plist` or `ReadingList.plist`)
- **Contextual search**: contact mentions topic → find relevant link to share

**Framing styles** (persona-configured):

```json
"link_sharing": {
  "frequency_per_week": 3,
  "framing_styles": [
    "no context - just the URL",
    "brief - 'check this out'",
    "editorial - 'this reminded me of what you said about X'"
  ],
  "default_framing": "brief",
  "topics_to_share": ["tech", "music", "food", "funny"]
}
```

**C API:**

```c
hu_error_t hu_visual_find_link(hu_allocator_t *alloc, hu_memory_t *memory,
                               const char *contact_id, size_t contact_id_len,
                               const char *topic, size_t topic_len,
                               char **url_out, size_t *url_len,
                               char **frame_out, size_t *frame_len);
```

### F118: Screenshot Capture

Programmatic screenshot of web content for sharing. When the system encounters something worth sharing visually (a tweet, a chart, a funny listing):

```c
hu_error_t hu_visual_capture_screenshot(hu_allocator_t *alloc,
                                        const char *url, size_t url_len,
                                        const char *output_path, size_t path_len);
```

Implementation: macOS `screencapture` CLI or WebKit thumbnail rendering via `wkwebview` snapshot. Guarded by `HU_IS_TEST`.

### F119: Visual Decision Engine

Central decision point: should the next message include visual content?

**Decision matrix:**

| Signal                    | Text Only | Text + Photo | Photo Only | Link | Screenshot |
| ------------------------- | --------- | ------------ | ---------- | ---- | ---------- |
| Just took a great photo   |           | ✓            |            |      |            |
| Contact mentioned a place |           | ✓            |            |      |            |
| Found relevant article    |           |              |            | ✓    |            |
| Funny web content         |           |              |            |      | ✓          |
| "Look at this" moment     |           |              | ✓          |      |            |
| Recipe/restaurant rec     |           |              |            | ✓    |            |

**Probability modifiers:**

- Close relationship → more visual sharing (2x)
- Late night → less visual sharing (0.5x)
- Contact doesn't send photos → reduce photo sharing (0.3x)
- Recent photo share → exponential cooldown

**C API:**

```c
typedef enum hu_visual_decision {
    HU_VISUAL_NONE,
    HU_VISUAL_PHOTO,
    HU_VISUAL_LINK,
    HU_VISUAL_SCREENSHOT,
    HU_VISUAL_PHOTO_WITH_TEXT,
    HU_VISUAL_LINK_WITH_TEXT
} hu_visual_decision_t;

hu_error_t hu_visual_decide(hu_allocator_t *alloc, hu_memory_t *memory,
                            const char *contact_id, size_t contact_id_len,
                            const char *conversation, size_t conv_len,
                            hu_visual_decision_t *out,
                            hu_visual_candidate_t *candidate);
```

### F120: Visual Context Matching

Embedding-based matching of visual content to conversation context and contact interests:

- Photo description embedding ↔ conversation topic embedding
- Contact interest keywords ↔ photo tags/descriptions
- Temporal relevance: recent photos weighted higher
- Social relevance: photos with shared contacts weighted higher

Uses existing `hu_memory_t` vector search infrastructure from the embeddings system.

### New SQLite Table

```sql
CREATE TABLE IF NOT EXISTS visual_content (
    id INTEGER PRIMARY KEY,
    source TEXT NOT NULL,          -- 'photos_app', 'screenshot', 'web'
    path TEXT NOT NULL,
    description TEXT,
    embedding BLOB,
    tags TEXT,                     -- JSON array of tags
    location TEXT,
    faces TEXT,                    -- JSON array of recognized face IDs
    captured_at INTEGER NOT NULL,
    indexed_at INTEGER NOT NULL,
    shared_with TEXT,              -- JSON array of contact_ids who've seen it
    share_count INTEGER DEFAULT 0
);
```

### Persona JSON

```json
"visual_content": {
  "enabled": true,
  "photos_library_path": "~/Pictures/Photos Library.photoslibrary",
  "scan_interval_hours": 6,
  "max_shares_per_contact_per_day": 2,
  "blocked_locations": ["medical", "legal"],
  "blocked_albums": ["Private"],
  "share_probability": 0.15,
  "link_sharing": {
    "frequency_per_week": 3,
    "framing_styles": ["bare_url", "brief", "editorial"],
    "default_framing": "brief",
    "topics_to_share": ["tech", "music", "food", "humor"]
  }
}
```

### New Module

`src/visual/content.c` (~400 lines) — photo scanning, matching, screenshot capture, decision engine.

---

## Pillar 19: Proactive Volume Governor

> Twenty proactive features firing independently will feel like a needy chatbot. Real humans have one intuitive sense: "Am I texting too much?"

### Features

| #    | Feature                                                                          | Complexity | Phase | Status         | Daemon          |
| ---- | -------------------------------------------------------------------------------- | ---------- | ----- | -------------- | --------------- |
| F121 | **Global proactive budget** — unified daily/weekly cap per contact               | High       | 2     | ✅ Implemented | ✅ Daemon Wired |
| F122 | **Priority queue arbitration** — rank all proactive candidates, pick top N       | Medium     | 2     | ✅ Implemented | ✅ Daemon Wired |
| F123 | **Reciprocity-aware throttling** — reduce outreach when contact isn't responding | Medium     | 3     | ✅ Implemented | ✅ Daemon Wired |
| F124 | **Busyness simulation** — sometimes too busy to text proactively                 | Low        | 4     | ✅ Implemented | ✅ Daemon Wired |

**Daemon integration:** Budget check before proactive messages; `hu_governor_has_budget()` gates proactive cycle; `hu_governor_record_sent()` after send.

### F121: Global Proactive Budget

Every proactive action (check-ins, follow-ups, bookends, curiosity, callbacks, visual shares, planning, emotional check-ins, narration, etc.) consumes from a single per-contact budget.

**Budget structure:**

```c
typedef struct hu_proactive_budget {
    uint8_t daily_max;          /* hard cap, default 3 */
    uint8_t weekly_max;         /* soft cap, default 10 */
    uint8_t daily_used;
    uint8_t weekly_used;
    uint64_t last_reset_day;    /* day number for daily reset */
    uint64_t last_reset_week;   /* week number for weekly reset */
    double relationship_multiplier; /* 0.5 for acquaintance, 1.0 for friend, 1.5 for partner */
} hu_proactive_budget_t;
```

**Effective budget** = `daily_max × relationship_multiplier` (from Relationship Dynamics model).

**Rules:**

- Daily budget resets at persona wake time (not midnight)
- Weekly budget resets Monday morning
- Responses to incoming messages do NOT consume budget (only unsolicited outreach)
- Emergency overrides: birthday, major life event → ignore budget for that one message
- Back-off: if contact hasn't responded to last 2 proactive messages → halve budget for 48 hours

**Integration with existing proactive system:**

The existing `hu_proactive_result_t` with its `actions[HU_PROACTIVE_MAX_ACTIONS]` array and priority sorting already provides the queue. The Governor wraps this:

```c
hu_error_t hu_governor_init(hu_allocator_t *alloc, const char *contact_id,
                            size_t contact_id_len, const hu_proactive_budget_config_t *config,
                            hu_proactive_budget_t *out);

/* Filter a proactive result through the budget. Mutates result in place,
   removing actions that exceed budget. Returns number of actions allowed. */
hu_error_t hu_governor_filter(hu_proactive_budget_t *budget,
                              hu_proactive_result_t *result,
                              uint64_t now_ms);

/* Record that a proactive action was sent (decrement budget). */
hu_error_t hu_governor_record_sent(hu_proactive_budget_t *budget, uint64_t now_ms);

/* Check if any budget remains for this contact right now. */
bool hu_governor_has_budget(const hu_proactive_budget_t *budget, uint64_t now_ms);
```

### F122: Priority Queue Arbitration

The existing `compare_priority_desc` in `proactive.c` sorts by priority. Extend with a richer priority model:

**Priority factors** (weighted sum):

| Factor           | Weight | Range   | Description                                                   |
| ---------------- | ------ | ------- | ------------------------------------------------------------- |
| Urgency          | 0.30   | 0.0–1.0 | Time-sensitive (birthday today = 1.0, random curiosity = 0.1) |
| Relevance        | 0.25   | 0.0–1.0 | How well this matches current conversation/contact state      |
| Freshness        | 0.20   | 0.0–1.0 | New information decays; stale prompts deprioritized           |
| Social debt      | 0.15   | 0.0–1.0 | Reciprocity imbalance (they initiated last 3 convos = high)   |
| Emotional weight | 0.10   | 0.0–1.0 | Emotional check-ins after heavy moments weighted higher       |

**C API:**

```c
double hu_governor_compute_priority(hu_proactive_action_type_t type,
                                    const hu_contact_profile_t *contact,
                                    const hu_relationship_state_t *rel_state,
                                    uint64_t now_ms);
```

### F123: Reciprocity-Aware Throttling

Feed reciprocity data from F63 (Social Reciprocity Tracking) into the Governor:

- If `initiation_ratio` > 0.7 (we initiated 70% of conversations), reduce budget by 40%
- If last 3 proactive messages got no response, enter "cool-off" (budget → 0 for 72 hours)
- If contact just initiated after a silence, boost budget temporarily (they're re-engaging)

### F124: Busyness Simulation

Some days the persona is just too busy to text proactively:

- Cross-reference with calendar (F50): busy calendar day → reduce budget by 50%
- Life simulation (F59): simulated busy/stressed state → reduce budget by 30%
- Random variance: 15% of days, budget reduced by 60% for no stated reason (humans are inconsistent)

### New SQLite Table

```sql
CREATE TABLE IF NOT EXISTS proactive_budget_log (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    date INTEGER NOT NULL,              -- day number
    budget_max INTEGER NOT NULL,
    budget_used INTEGER NOT NULL,
    actions_sent TEXT,                   -- JSON array of action types sent
    actions_suppressed INTEGER DEFAULT 0 -- how many were filtered out
);
```

### Persona JSON

```json
"proactive_governor": {
  "daily_max_default": 3,
  "weekly_max_default": 10,
  "relationship_multipliers": {
    "partner": 2.0,
    "close_friend": 1.5,
    "friend": 1.0,
    "acquaintance": 0.5,
    "coworker": 0.3
  },
  "cool_off_after_unanswered": 2,
  "cool_off_hours": 72,
  "busyness_calendar_reduction": 0.5,
  "random_low_days_probability": 0.15
}
```

### New Module

`src/agent/governor.c` (~300 lines) — budget management, priority computation, reciprocity integration.

---

## Pillar 20: Contact Knowledge State

> Real humans never say "Did I tell you about this already?" without actually being unsure. They have a fuzzy but functional model of who knows what. Without this, the system WILL tell the same story to the same person twice, and it WILL leak information between contacts.

### Features

| #    | Feature                                                                           | Complexity | Phase | Status         | Daemon          |
| ---- | --------------------------------------------------------------------------------- | ---------- | ----- | -------------- | --------------- |
| F125 | **Knowledge graph per contact** — track what each contact knows about your life   | High       | 3     | ✅ Implemented | ✅ Daemon Wired |
| F126 | **Disclosure tracking** — record what was shared, when, and to whom               | Medium     | 3     | ✅ Implemented | ✅ Daemon Wired |
| F127 | **Redundancy detection** — prevent telling the same story twice                   | Medium     | 3     | ✅ Implemented | ✅ Daemon Wired |
| F128 | **Cross-contact isolation** — enforce information boundaries between contacts     | High       | 3     | ✅ Implemented | ✅ Daemon Wired |
| F129 | **"Did I tell you?" pattern** — naturalistic uncertainty about what's been shared | Low        | 6     | ✅ Implemented | ✅ Daemon Wired |

**Daemon integration:** Knowledge query and summary prompt in Phase 6 context; `hu_knowledge_query_sql()` + `hu_knowledge_build_summary()` + `hu_knowledge_summary_to_prompt()`.

### F125: Knowledge Graph Per Contact

Every fact about the persona's life has a set of contacts who know it. This is the inverse of memory — not "what do I remember about them" but "what do they know about me."

**Knowledge entry model:**

```c
typedef struct hu_knowledge_entry {
    int64_t id;
    char *topic;              /* "job_interview", "new_apartment", "health_issue" */
    size_t topic_len;
    char *detail;             /* "I had an interview at Google on Tuesday" */
    size_t detail_len;
    char *contact_id;         /* who knows this */
    size_t contact_id_len;
    uint64_t shared_at;       /* when it was shared */
    char *source;             /* "conversation", "group_chat", "inferred" */
    size_t source_len;
    double confidence;        /* 1.0 = explicitly told them, 0.5 = they might know */
} hu_knowledge_entry_t;
```

**Population:**

- **Explicit sharing**: After sending a message that contains personal information, extract facts via LLM and record them with the recipient
- **Group chat sharing**: If shared in a group, all group members gain knowledge (confidence = 0.8)
- **Inferred knowledge**: If contact A is close friends with contact B, and you told A something, B _might_ know (confidence = 0.3)
- **Social media posts**: Anything posted publicly → all contacts know (confidence = 0.9)

### F126: Disclosure Tracking

Every outbound message is processed for personal disclosures:

```c
hu_error_t hu_knowledge_extract_disclosures(hu_allocator_t *alloc,
                                            const char *message, size_t msg_len,
                                            const char *contact_id, size_t contact_id_len,
                                            hu_knowledge_entry_t **out, size_t *out_count);

hu_error_t hu_knowledge_record(hu_memory_t *memory,
                               const hu_knowledge_entry_t *entries, size_t count);
```

Extraction is LLM-powered during the conversation flow — lightweight, not a separate pass. The LLM response already goes through post-processing; add disclosure extraction there.

### F127: Redundancy Detection

Before sharing personal information, check if the contact already knows:

```c
/* Returns true if contact likely already knows about this topic. */
bool hu_knowledge_contact_knows(hu_memory_t *memory,
                                const char *contact_id, size_t contact_id_len,
                                const char *topic, size_t topic_len,
                                double min_confidence);
```

**Prompt injection**: When the Context Arbitrator builds the prompt, it includes:

```
[KNOWLEDGE STATE for {contact}]:
- They know about: your job interview, your new apartment, the trip to Austin
- They DON'T know about: your health concern, the argument with Dad
- Uncertain: whether they know about the car repair (mentioned in group chat)

RULE: Do not re-tell stories they already know. If you want to reference something, assume they remember. If genuinely unsure, use "did I tell you about X?" framing.
```

### F128: Cross-Contact Isolation

The most critical safety feature. Information shared with contact A must never leak to contact B unless:

- It was shared in a group containing both
- It's public information (social media post)
- It's general knowledge (job title, where you live)

```c
/* Check if sharing topic with target_contact would leak info from source_contact. */
bool hu_knowledge_would_leak(hu_memory_t *memory,
                             const char *topic, size_t topic_len,
                             const char *source_contact_id, size_t source_len,
                             const char *target_contact_id, size_t target_len);
```

This feeds directly into F68 (Protective Intelligence) as a hard gate. The Arbitrator will NEVER inject information that fails this check.

### F129: "Did I Tell You?" Pattern

Naturalistic uncertainty. When the Knowledge State confidence is between 0.3 and 0.7:

- 40% of the time: "wait did I tell you about [X]?"
- 30%: Just tell them anyway (humans repeat themselves sometimes)
- 30%: Skip it (assume they know)

This is a Phase 6 feature because it requires the self-awareness system (F62) to generate natural uncertainty.

### New SQLite Table

```sql
CREATE TABLE IF NOT EXISTS contact_knowledge (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    detail TEXT,
    confidence REAL NOT NULL DEFAULT 1.0,
    source TEXT NOT NULL,                     -- 'direct', 'group_chat', 'inferred', 'public'
    shared_at INTEGER NOT NULL,
    source_contact_id TEXT,                   -- if inferred, who did we originally tell
    UNIQUE(contact_id, topic)                 -- one entry per contact per topic
);
```

### New Module

`src/memory/knowledge.c` (~350 lines) — knowledge graph CRUD, disclosure extraction, redundancy check, isolation enforcement.

---

## Pillar 21: Collaborative Planning

> Humans initiate plans. "We should grab dinner." "Are you free this weekend?" "I just saw tickets for [band]." A system that only responds to and follows up on plans but never creates them is observably passive.

### Features

| #    | Feature                                                                               | Complexity | Phase | Status         |
| ---- | ------------------------------------------------------------------------------------- | ---------- | ----- | -------------- |
| F130 | **Plan initiation** — proactively suggest activities based on shared interests        | High       | 4     | ✅ Implemented |
| F131 | **Logistics coordination** — follow through on plan details (time, place, who)        | Medium     | 4     | ✅ Implemented |
| F132 | **Opportunistic triggers** — share events/deals/openings that match contact interests | Medium     | 4     | ✅ Implemented |
| F133 | **Plan memory** — track proposed/accepted/rejected/completed plans                    | Medium     | 3     | ✅ Implemented |

### F130: Plan Initiation

The system should proactively suggest getting together, doing activities, or making plans based on:

- **Shared interests**: Contact likes hiking + good weather this weekend → "we should hit [trail] Saturday"
- **Temporal patterns**: You and contact always hang out ~every 2 weeks → approaching that window → suggest something
- **Seasonal/event**: Concert coming up, new restaurant opened, holiday approaching
- **Relationship maintenance**: Haven't hung out in a while → "it's been a minute, we should do something"

**Constraints:**

- Only for contacts with relationship type `friend` or closer
- Maximum 2 plan proposals per contact per month (governed by proactive budget)
- Never propose plans during detected stress/crisis periods
- Respect recent rejection cooldown (if last plan was declined, wait 2+ weeks)

**C API:**

```c
typedef struct hu_plan_proposal {
    char *activity;
    size_t activity_len;
    char *suggested_time;       /* "this weekend", "next Friday", "sometime soon" */
    size_t suggested_time_len;
    char *reasoning;            /* why this plan makes sense (for LLM context) */
    size_t reasoning_len;
    double confidence;
} hu_plan_proposal_t;

hu_error_t hu_planning_generate_proposal(hu_allocator_t *alloc, hu_memory_t *memory,
                                         const char *contact_id, size_t contact_id_len,
                                         const hu_contact_profile_t *contact,
                                         hu_plan_proposal_t *out);
```

### F131: Logistics Coordination

Once a plan is proposed and accepted, track and coordinate:

- **Time negotiation**: "What time works?" → remember their answer
- **Location**: "Where should we go?" → suggest based on history and preferences
- **Confirmation**: Day-of reminder: "still on for tonight?"
- **Cancellation handling**: Natural: "hey something came up, can we reschedule?"

This extends F50 (Calendar awareness) by writing proposed plans into the `commitments` table with type = `plan`:

```c
hu_error_t hu_planning_track(hu_memory_t *memory, const char *contact_id,
                             size_t contact_id_len, const char *plan_description,
                             size_t desc_len, int64_t proposed_time,
                             const char *status);  /* 'proposed', 'accepted', 'confirmed', 'completed', 'cancelled' */
```

### F132: Opportunistic Triggers

When external feeds (F83–F93) surface something matching a contact's interests:

- Concert announcement for a band they mentioned → "dude [band] is coming to Austin in April, we should go"
- Restaurant in their neighborhood → "have you tried [place]? looks amazing"
- Article about their hobby → "saw this and thought of you" (link share via F117)

This is the bridge between the external awareness system (Phase 7) and the planning system. Feed items are scored against contact interest profiles and surfaced as plan triggers.

### F133: Plan Memory

Track the full lifecycle of plans:

```sql
CREATE TABLE IF NOT EXISTS plans (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    activity TEXT NOT NULL,
    proposed_at INTEGER NOT NULL,
    proposed_by TEXT NOT NULL,           -- 'self' or 'contact'
    status TEXT DEFAULT 'proposed',      -- proposed, accepted, confirmed, completed, cancelled, declined
    scheduled_time INTEGER,
    location TEXT,
    reminder_sent INTEGER DEFAULT 0,
    completed_at INTEGER,
    outcome_notes TEXT                   -- "great time" / "they cancelled last minute"
);
```

**Plan analytics** feed back into Relationship Dynamics (Pillar 23):

- Declining plan rate increasing → relationship may be cooling
- Always accepting → good health, consider proposing more
- Frequent cancellations → something going on, don't push

### Persona JSON

```json
"planning": {
  "enabled": true,
  "max_proposals_per_contact_per_month": 2,
  "rejection_cooldown_days": 14,
  "interests_to_activities": {
    "hiking": ["trail runs", "day hikes", "camping trips"],
    "food": ["new restaurants", "cooking together", "food festivals"],
    "music": ["concerts", "vinyl shopping", "music festivals"],
    "sports": ["watching games", "pickup basketball", "golf"]
  },
  "default_suggest_style": "casual",
  "day_of_confirmation": true
}
```

### New Module

`src/agent/planning.c` (~350 lines) — plan proposal generation, lifecycle tracking, opportunistic matching.

---

## Pillar 22: Context Arbitration Layer

> Injecting 20 simultaneous directives into an LLM prompt produces a schizophrenic response. Humans naturally prioritize: right now, what matters most? The Arbitrator is the brain's executive function for the prompt.

### Features

| #    | Feature                                                                                | Complexity | Phase | Status         |
| ---- | -------------------------------------------------------------------------------------- | ---------- | ----- | -------------- |
| F134 | **Directive priority ranking** — score and rank all candidate prompt injections        | High       | 2     | ✅ Implemented |
| F135 | **Token budget management** — fit maximum signal into fixed context window             | Medium     | 2     | ✅ Implemented |
| F136 | **Conflict resolution** — detect and resolve contradictory directives                  | High       | 2     | ✅ Implemented |
| F137 | **Dynamic directive selection** — select top-K directives per message based on context | Medium     | 2     | ✅ Implemented |

### F134: Directive Priority Ranking

Every system that injects context into the prompt produces a **directive** — a structured instruction competing for prompt space. The Arbitrator scores each one.

**Directive model:**

```c
typedef struct hu_directive {
    char *content;
    size_t content_len;
    char *source;               /* "energy_match", "emotional_checkin", "inside_joke", etc. */
    size_t source_len;
    double priority;            /* computed score 0.0–1.0 */
    double token_cost;          /* estimated tokens */
    uint32_t category;          /* bitfield: EMOTIONAL, BEHAVIORAL, MEMORY, PROACTIVE, SAFETY */
    bool required;              /* safety directives are never filtered out */
} hu_directive_t;

#define HU_DIRECTIVE_EMOTIONAL   (1u << 0)
#define HU_DIRECTIVE_BEHAVIORAL  (1u << 1)
#define HU_DIRECTIVE_MEMORY      (1u << 2)
#define HU_DIRECTIVE_PROACTIVE   (1u << 3)
#define HU_DIRECTIVE_SAFETY      (1u << 4)
#define HU_DIRECTIVE_IDENTITY    (1u << 5)
```

**Priority factors:**

| Factor             | Description                                                      |
| ------------------ | ---------------------------------------------------------------- |
| Recency            | Directives about the current message context rank highest        |
| Emotional weight   | High-emotion situations boost emotional directives               |
| Relationship stage | Deep relationships → more memory/knowledge directives            |
| Conversation phase | Early in convo → identity/warmth; deep in convo → content/memory |
| Novelty            | First-time situations rank higher than recurring patterns        |

### F135: Token Budget Management

Fixed token budget for injected directives (configurable, default 1500 tokens):

```c
typedef struct hu_arbitration_config {
    size_t max_directive_tokens;    /* default 1500 */
    size_t max_directives;         /* default 7 */
    bool allow_compression;        /* use shared compression to shrink directives */
} hu_arbitration_config_t;

hu_error_t hu_arbitrator_select(hu_allocator_t *alloc,
                                const hu_directive_t *candidates, size_t candidate_count,
                                const hu_arbitration_config_t *config,
                                hu_directive_t **selected, size_t *selected_count);
```

**Selection algorithm:**

1. Partition into required (safety, core identity) and optional
2. Reserve tokens for required directives
3. Sort optional by priority descending
4. Greedy knapsack: add directives until token budget exhausted
5. If a directive is large but high-priority, attempt compression via Shared Experience Compression
6. Return selected set

### F136: Conflict Resolution

Detect contradictory directives and resolve:

| Conflict                                             | Resolution                                  |
| ---------------------------------------------------- | ------------------------------------------- |
| "Be enthusiastic" + "Be calm and gentle"             | Emotional state wins — check energy match   |
| "Reference inside joke" + "This is a serious moment" | Emotional context wins — suppress humor     |
| "Share about your day" + "They're venting, listen"   | Active listening wins — defer self-sharing  |
| "Ask about X" + "They're avoiding X"                 | Avoidance pattern wins — suppress curiosity |

**Implementation**: Conflict pairs encoded as a static lookup table. When both sides of a conflict are in the candidate set, the higher-priority one suppresses the other.

```c
typedef struct hu_directive_conflict {
    const char *source_a;
    const char *source_b;
    const char *winner;     /* which source wins, or "context" for dynamic resolution */
} hu_directive_conflict_t;

size_t hu_arbitrator_resolve_conflicts(hu_directive_t *directives, size_t count);
```

### F137: Dynamic Directive Selection

The number of directives scales with conversation complexity:

- Simple exchange ("hey" / "hey what's up") → 2-3 directives (identity + energy)
- Normal conversation → 4-5 directives
- Emotionally complex → 5-7 directives (add emotional, protective, knowledge state)
- Crisis → 2-3 directives (strip to essentials: emotional support + safety)

**Integration point**: The Arbitrator runs inside `hu_persona_build_prompt()` as a new stage between directive collection and prompt assembly.

### Persona JSON

```json
"context_arbitration": {
  "max_directive_tokens": 1500,
  "max_directives": 7,
  "compression_enabled": true,
  "conflict_rules": [
    {"if": ["humor", "serious_moment"], "keep": "serious_moment"},
    {"if": ["self_sharing", "active_listening"], "keep": "active_listening"},
    {"if": ["curiosity_about_X", "avoidance_of_X"], "keep": "avoidance_of_X"}
  ]
}
```

### New Module

`src/agent/arbitrator.c` (~400 lines) — directive scoring, token budgeting, conflict resolution, selection.

---

## Pillar 23: Relationship Dynamics Model

> The original plan has static relationship stages: acquaintance → friend → close_friend → partner. Real relationships have velocity, trajectory, and oscillation. You sense when a friendship is cooling before either person says anything.

### Features

| #    | Feature                                                                  | Complexity | Phase | Status         |
| ---- | ------------------------------------------------------------------------ | ---------- | ----- | -------------- |
| F138 | **Relationship velocity tracking** — measure rate of change in closeness | High       | 6     | ✅ Implemented |
| F139 | **Drift detection** — identify when a relationship is cooling            | Medium     | 6     | ✅ Implemented |
| F140 | **Repair mode** — adjust behavior when relationship is strained          | Medium     | 6     | ✅ Implemented |

### F138: Relationship Velocity Tracking

Model relationship state as a continuous variable with velocity, not a discrete stage:

```c
typedef struct hu_relationship_state {
    char *contact_id;
    size_t contact_id_len;
    double closeness;           /* 0.0 (stranger) to 1.0 (intimate) */
    double velocity;            /* rate of change: positive = deepening, negative = drifting */
    double vulnerability_depth; /* how deep conversations have gone: 0.0–1.0 */
    double reciprocity;         /* -1.0 (they give all) to 1.0 (we give all); 0.0 = balanced */
    uint64_t last_interaction;
    uint64_t last_vulnerability_moment;
    char *current_mode;         /* "normal", "deepening", "drifting", "repair", "reconnecting" */
    size_t current_mode_len;
} hu_relationship_state_t;
```

**Signal inputs** (measured over rolling 30-day window vs prior 30 days):

| Signal                            | Weight | What It Measures                                 |
| --------------------------------- | ------ | ------------------------------------------------ |
| Message frequency delta           | 0.20   | Are we talking more or less?                     |
| Initiation ratio delta            | 0.15   | Who starts conversations? Shifting?              |
| Response time delta               | 0.15   | Are they slower to respond? Are we?              |
| Message length delta              | 0.10   | Shorter messages = lower engagement              |
| Vulnerability depth delta         | 0.15   | Deeper sharing = deepening; shallow = plateauing |
| Plan completion rate              | 0.10   | Making and keeping plans = strong signal         |
| Laughter/positive sentiment delta | 0.10   | Emotional tone trending up or down               |
| Topic diversity delta             | 0.05   | Narrow topics = drifting toward transactional    |

**Velocity computation:**

```c
hu_error_t hu_relationship_compute_state(hu_allocator_t *alloc, hu_memory_t *memory,
                                         const char *contact_id, size_t contact_id_len,
                                         hu_relationship_state_t *out);
```

Run weekly (not per-message — too noisy). Store results in SQLite for trend analysis.

### F139: Drift Detection

When velocity is negative for 2+ consecutive measurement periods:

- **Early drift** (velocity -0.1 to -0.3): Subtle behavioral adjustment — slightly increase initiation, ask about their life more
- **Clear drift** (velocity < -0.3): Honest acknowledgment if appropriate: "feels like it's been a minute — how are you really doing?"
- **Mutual drift** (both sides reducing): Accept gracefully — don't fight natural relationship evolution. Reduce proactive budget proportionally.
- **One-sided drift** (they're pulling away): Reduce proactive outreach, give space, but keep the door open

**Governor integration**: Relationship velocity feeds directly into the Proactive Governor's budget multiplier. Drifting relationships get smaller budgets — the system doesn't chase.

### F140: Repair Mode

When the system detects strain (negative interaction quality from F115, or velocity drop after conflict detected by F14):

**Repair mode behaviors:**

- Reduce humor and playfulness
- Increase warmth and directness
- Don't pretend nothing happened
- Acknowledge without over-apologizing: "hey, I've been off lately" / "sorry about earlier"
- Give space: extend response times, reduce proactive messages
- Watch for repair signals from the contact (re-engagement, humor, vulnerability)
- Auto-exit repair mode when signals normalize for 3+ days

```c
hu_error_t hu_relationship_enter_repair(hu_memory_t *memory, const char *contact_id,
                                        size_t contact_id_len, const char *reason,
                                        size_t reason_len);

bool hu_relationship_in_repair(hu_memory_t *memory, const char *contact_id,
                               size_t contact_id_len);

/* Check if repair signals detected — should we exit repair mode? */
hu_error_t hu_relationship_check_repair_exit(hu_memory_t *memory, const char *contact_id,
                                             size_t contact_id_len, bool *should_exit);
```

### New SQLite Table

```sql
CREATE TABLE IF NOT EXISTS relationship_state (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    closeness REAL NOT NULL,
    velocity REAL NOT NULL,
    vulnerability_depth REAL NOT NULL,
    reciprocity REAL NOT NULL,
    current_mode TEXT DEFAULT 'normal',
    mode_entered_at INTEGER,
    measured_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_relstate_contact ON relationship_state(contact_id, measured_at);
```

### Persona JSON

```json
"relationship_dynamics": {
  "enabled": true,
  "measurement_interval_days": 7,
  "drift_threshold_velocity": -0.1,
  "clear_drift_threshold": -0.3,
  "repair_auto_enter": true,
  "repair_exit_after_days": 3,
  "drift_budget_multiplier": 0.5,
  "signals": {
    "frequency_weight": 0.20,
    "initiation_weight": 0.15,
    "response_time_weight": 0.15,
    "message_length_weight": 0.10,
    "vulnerability_weight": 0.15,
    "plan_completion_weight": 0.10,
    "sentiment_weight": 0.10,
    "topic_diversity_weight": 0.05
  }
}
```

### New Module

`src/agent/relationship.c` (~400 lines) — state computation, drift detection, repair mode management.

---

## Pillar 24: Shared Experience Compression

> Close friends communicate in a private language. A single word or image can carry an entire shared memory. "The Incident." "Your nemesis." "The place." This ultra-compressed communication is the strongest signal of genuine closeness.

### Features

| #    | Feature                                                                                   | Complexity | Phase | Status         | Daemon          |
| ---- | ----------------------------------------------------------------------------------------- | ---------- | ----- | -------------- | --------------- |
| F141 | **Shared reference tracking** — detect and catalog compressed references between contacts | High       | 6     | ✅ Implemented | ✅ Daemon Wired |
| F142 | **Compression generation** — create new compressed references from shared experiences     | Medium     | 8     | ✅ Implemented | ✅ Daemon Wired |
| F143 | **Reference deployment** — use compressed references naturally in conversation            | Medium     | 6     | ✅ Implemented | ✅ Daemon Wired |

**Daemon integration:** Compression query and prompt in Phase 6 context; `hu_compression_query_sql()` + `hu_compression_build_prompt()`.

### F141: Shared Reference Tracking

A shared reference is any word, phrase, or concept that carries meaning only in the context of two specific people's relationship. These emerge organically from conversation:

- **Proper noun references**: "the incident at Dave's" → both know what happened
- **Inside nicknames**: "your nemesis" = a specific person they both know
- **Abbreviated experiences**: "like last time" = a specific shared memory
- **Catchphrases**: Phrases that became running bits between two people
- **One-word triggers**: A single word that activates a shared memory ("mango" → the time they got food poisoning)

**Detection:**

The LLM, with conversation history, can identify when a reference is being used with compressed meaning. Key signals:

- A noun phrase that has no antecedent in the current conversation but both parties understand
- A phrase that previously generated strong positive/humorous response
- A callback reference that was reinforced (used, laughed at, built upon)

```c
typedef struct hu_shared_reference {
    int64_t id;
    char *contact_id;
    size_t contact_id_len;
    char *compressed_form;      /* "the incident", "your nemesis", "mango" */
    size_t compressed_form_len;
    char *expanded_meaning;     /* the full shared memory this refers to */
    size_t expanded_meaning_len;
    uint32_t usage_count;       /* how many times it's been used */
    uint64_t created_at;
    uint64_t last_used_at;
    double strength;            /* 0.0–1.0: how established this reference is */
} hu_shared_reference_t;

hu_error_t hu_compression_detect(hu_allocator_t *alloc, hu_memory_t *memory,
                                 const char *contact_id, size_t contact_id_len,
                                 const char *message, size_t msg_len,
                                 hu_shared_reference_t **detected, size_t *count);

hu_error_t hu_compression_record(hu_memory_t *memory,
                                 const hu_shared_reference_t *ref);
```

### F142: Compression Generation

The system can create new compressed references by:

1. **Noticing compressible moments**: A funny/meaningful shared experience → after it's referenced 2+ times, formalize it as a shared reference
2. **Natural compression**: First use: full story. Second reference: abbreviated. Third: single phrase. This mirrors how humans naturally compress shared experiences.
3. **Testing compression**: Use a slightly compressed version and see if the contact responds with recognition → confirms the reference is established

**Lifecycle:**

```
Full story → "remember when X" → "like that time" → "another X situation" → just "X"
```

Each stage requires the contact to demonstrate recognition (responding naturally without confusion) before the system compresses further.

### F143: Reference Deployment

When to use a compressed reference in conversation:

- **Analogies**: Current situation mirrors a shared experience → "this is giving [reference] energy"
- **Humor**: Callback to a funny shared moment → just the compressed form, no explanation
- **Bonding**: Using shared language reinforces closeness → deploy more during positive moments
- **Never during conflict**: Compressed references require emotional alignment to land correctly

**Prompt injection** (via Context Arbitrator):

```
[SHARED LANGUAGE with {contact}]:
- "mango" = the food poisoning incident in Mexico (strength: 0.9, used 7 times)
- "your nemesis" = their coworker Kevin (strength: 0.8, used 4 times)
- "the plan" = their running joke about opening a taco truck (strength: 0.6, used 2 times)

You may use these naturally when relevant. Never explain them — if you use one, commit to the compressed form.
```

### New SQLite Table

```sql
CREATE TABLE IF NOT EXISTS shared_references (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    compressed_form TEXT NOT NULL,
    expanded_meaning TEXT NOT NULL,
    usage_count INTEGER DEFAULT 1,
    strength REAL DEFAULT 0.3,
    created_at INTEGER NOT NULL,
    last_used_at INTEGER,
    compression_stage INTEGER DEFAULT 1,  -- 1=full, 2=abbreviated, 3=compressed, 4=single-word
    UNIQUE(contact_id, compressed_form)
);
```

### Persona JSON

```json
"shared_compression": {
  "enabled": true,
  "min_uses_to_compress": 2,
  "max_references_per_contact": 20,
  "deployment_probability": 0.3,
  "never_during_conflict": true,
  "strength_decay_rate": 0.02
}
```

### New Module

`src/memory/compression.c` (~300 lines) — reference detection, lifecycle management, deployment selection.

---

## Integration Map: Where Each System Plugs In

### Daemon Proactive Cycle (`src/daemon.c`)

Current flow:

```
hourly trigger → jitter → per-contact loop →
    silence check → temporal check → event check →
    important dates → curiosity → callbacks →
    LLM prompt → response → send
```

New flow (additions in **bold**):

```
hourly trigger → jitter → per-contact loop →
    **relationship state check** →
    **governor budget check (early exit if 0)** →
    silence check → temporal check → event check →
    important dates → curiosity → callbacks →
    **plan initiation check** →
    **visual content check** →
    **ALL candidates → governor priority filter** →
    **context arbitrator builds prompt** →
    LLM prompt → response →
    **disclosure tracking (post-send)** →
    send
```

### Conversation Context Builder (`src/context/conversation.c`)

Current: Builds awareness context from emotional state, style, memory.

New additions:

1. **Knowledge State injection** — what this contact knows about us
2. **Shared Reference injection** — compressed language for this contact
3. **Directive collection** — all context systems produce directives
4. **Arbitrator selection** — top-K directives within token budget
5. **Relationship mode injection** — "normal", "repair", "reconnecting" affects tone

### Persona Prompt Builder (`src/persona/persona.c`)

Add directive arbitration as a stage in `hu_persona_build_prompt()`:

```
identity section → channel overlay → contact context →
    **knowledge state** → **shared references** →
    **directive collection from all systems** →
    **arbitrator: score, resolve conflicts, select top-K** →
    example selection → final prompt assembly
```

### Extended Pillars — Daemon Wiring (25, 29, 31, 32)

Additional modules wired into `daemon.c` beyond Pillars 18–24:

| Pillar | Module                     | Daemon Integration                                 | Status          |
| ------ | -------------------------- | -------------------------------------------------- | --------------- |
| 25     | Persona Fine-Tuning / LoRA | Training sample collection in post-turn processing | ✅ Daemon Wired |
| 29     | On-Device Classification   | Message classification in Phase 6 context          | ✅ Daemon Wired |
| 31     | Statistical Timing Model   | Timing model overlay on reading delay              | ✅ Daemon Wired |
| 32     | Behavioral Cloning         | Feedback recording in post-turn processing         | ✅ Daemon Wired |

---

## Phase Assignment

These 28 features integrate into the existing 9-phase structure:

| Phase                      | New Features                                         | Rationale                                                                                                                           |
| -------------------------- | ---------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| 2 (Emotional Intelligence) | F121, F122, F134, F135, F136, F137                   | Governor and Arbitrator are prerequisites for all later proactive features — must exist before Phase 3+ adds more proactive actions |
| 3 (Superhuman Memory)      | F123, F125, F126, F127, F128, F133                   | Knowledge State and Plan Memory are memory systems; Reciprocity throttling uses memory data                                         |
| 4 (Behavioral Polish)      | F116, F117, F118, F119, F120, F124, F130, F131, F132 | Visual content and planning are behavioral polish features; busyness simulation is behavioral                                       |
| 6 (AGI Cognition)          | F129, F138, F139, F140, F141, F143                   | Relationship dynamics and shared compression require the cognition layer; "Did I tell you?" needs self-awareness                    |
| 8 (Skill Acquisition)      | F142                                                 | Compression generation is a learned skill — the system gets better at creating shared language over time                            |

### Revised Phase Map

| Phase     | Original Features                               | New Features                   | New Estimated Lines  |
| --------- | ----------------------------------------------- | ------------------------------ | -------------------- |
| 1         | F1-F7, F10-F11, F15, F40-F44                    | —                              | ~800                 |
| 2         | F13-F14, F16-F17, F25, F27, F29, F33, F45-F46   | **F121-F122, F134-F137**       | ~600 + ~600 = ~1200  |
| 3         | F18-F24, F26, F30-F31, F50, F53                 | **F123, F125-F128, F133**      | ~1200 + ~500 = ~1700 |
| 4         | F8-F9, F12, F28, F32, F47-F49, F51-F52, F54-F57 | **F116-F120, F124, F130-F132** | ~900 + ~700 = ~1600  |
| 5         | F34-F39                                         | —                              | ~700                 |
| 6         | F58-F69                                         | **F129, F138-F141, F143**      | ~1800 + ~600 = ~2400 |
| 7         | F70-F76, F83-F93                                | —                              | ~2200                |
| 8         | F77-F82, F94-F101                               | **F142**                       | ~1600 + ~100 = ~1700 |
| 9         | F102-F115                                       | —                              | ~1200                |
| **Total** | **115 features**                                | **+28 features = 143**         | **~13500 lines**     |

---

## New Success Criteria

Add to the existing 14 criteria:

| #   | Test                                                                                                                        | Threshold                              |
| --- | --------------------------------------------------------------------------------------------------------------------------- | -------------------------------------- |
| 15  | **Visual test**: System sends photos/links/screenshots naturally; recipient finds them relevant                             | >70% relevance rating                  |
| 16  | **Volume test**: No contact receives >3 unsolicited messages per day on average over a week                                 | Hard cap enforced                      |
| 17  | **Knowledge test**: System never tells the same story to the same person twice                                              | 0 detected redundancies in 1-week test |
| 18  | **Isolation test**: System never leaks information between contacts                                                         | 0 leaks in adversarial testing         |
| 19  | **Planning test**: System proactively suggests plans that contact finds appealing                                           | >50% acceptance rate                   |
| 20  | **Coherence test**: LLM responses never show conflicting behavioral signals (funny AND somber, curious AND avoidant)        | 0 contradictions in 100-message audit  |
| 21  | **Drift test**: System correctly identifies relationship cooling >60% of the time before explicit signals                   | 60%+ detection rate                    |
| 22  | **Compression test**: System uses established shared references naturally; contact responds with recognition, not confusion | >80% recognition rate                  |

---

## Risk Assessment (New Risks)

| Risk                                                           | Probability | Impact           | Mitigation                                                       |
| -------------------------------------------------------------- | ----------- | ---------------- | ---------------------------------------------------------------- |
| Apple Photos SQLite schema changes between macOS versions      | Medium      | High (F116)      | Version detection, graceful fallback to disabled                 |
| Governor too restrictive → system feels distant                | Medium      | Medium (F121)    | A/B test budget levels, start generous and tighten               |
| Governor too permissive → system feels needy                   | Medium      | High (F121)      | Conservative defaults, relationship-type scaling                 |
| Knowledge extraction hallucination → false disclosure tracking | Medium      | Medium (F126)    | High-confidence threshold, only extract explicit statements      |
| Cross-contact leak despite isolation check                     | Low         | Very High (F128) | Defense in depth: LLM gate + SQL check + prompt instruction      |
| Relationship velocity noise → false drift detection            | Medium      | Medium (F139)    | 30-day rolling window, 2+ consecutive negative readings required |
| Shared reference used when contact has forgotten it            | Low         | Low (F143)       | Strength decay, strength threshold for deployment                |
| Plan proposals feel robotic/forced                             | Medium      | Medium (F130)    | LLM generates proposal, natural casual tone, persona-styled      |
| Arbitrator removes critical directives                         | Low         | High (F134)      | Safety directives marked `required`, never filtered              |
| Token budget too tight → important context dropped             | Medium      | Medium (F135)    | Monitor dropped directives, adaptive budget expansion            |

---

## Implementation Dependencies

```
Phase 2 (must come first):
    F121 (Governor) ← prerequisite for all proactive features
    F134-F137 (Arbitrator) ← prerequisite for coherent prompts

Phase 3 (depends on Phase 2):
    F125-F128 (Knowledge State) ← depends on Arbitrator for prompt injection
    F133 (Plan Memory) ← depends on Governor for budget
    F123 (Reciprocity Throttling) ← depends on Governor

Phase 4 (depends on Phase 2-3):
    F116-F120 (Visual Pipeline) ← depends on Governor for rate limiting
    F130-F132 (Planning) ← depends on Plan Memory, Governor, Knowledge State
    F124 (Busyness) ← depends on Governor

Phase 6 (depends on Phase 3):
    F138-F140 (Relationship Dynamics) ← depends on Governor, Knowledge State
    F141, F143 (Shared Compression) ← depends on Knowledge State
    F129 ("Did I tell you?") ← depends on Knowledge State

Phase 8 (depends on Phase 6):
    F142 (Compression Generation) ← depends on Shared Reference Tracking
```

---

## Summary: What Changes in the System's Behavior

**Before (original 115 features):**

- System responds well to incoming messages
- System proactively checks in, but with no volume control
- System never sends photos, links, or visual content
- System may tell the same story twice
- System may leak information between contacts
- System never proposes plans
- Prompt contains conflicting instructions, leading to incoherent responses
- Relationships are modeled as static stages
- No ultra-compressed shared language

**After (143 features, 7 new pillars):**

- System shares photos of a sunset it "took" yesterday, sends relevant links, shares screenshots of funny things
- System texts proactively but never too much — some days barely at all (like a real person)
- System remembers who knows what and never repeats stories or leaks information
- System suggests getting dinner, buying concert tickets, going hiking
- Every response is governed by a coherent set of prioritized directives that don't contradict each other
- System senses when a friendship is drifting and adjusts naturally — gives space, re-engages gently
- System develops private language with each close contact — compressed references that carry entire shared memories in a word

This is the difference between "AI that texts well" and "person who texts, backed by superhuman intelligence."
