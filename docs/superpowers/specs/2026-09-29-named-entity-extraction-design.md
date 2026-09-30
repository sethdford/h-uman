---
title: Named-entity extraction — per-turn catcher + nightly typed pass
status: approved
date: 2026-09-29
---

# Named-entity extraction — per-turn catcher + nightly typed pass

> **Amended 2026-09-30** to match what ships (rulings in the SDD ledger,
> `.superpowers/sdd/2026-09-29-named-entity-extraction/progress.md`). The success
> gate in §2 is now the paired flip gate of §4.7 and the operator guide; §4.4's
> nightly step runs **before** the wide pass; §6 is the reordered rollout. The
> catcher, the migration and a `--write` nightly pass all change what is sent as
> soon as they write: their rows reach replies through lexical and
> contact-fallback grounding (fallback is LIVE in prod) whatever `HU_GRAPH_NAMES`
> says. `HU_GRAPH_NAMES` governs only typed-name selection. Operator detail:
> `docs/guides/named-entities.md`.

## 1. Problem (measured 2026-09-29, prod graph.db, read-only counts)

Replies are generic where Seth is specific because the graph never learns the
names his contacts talk about. Nothing live writes them:

| Measurement | Value |
|---|---|
| Prod `llm_decides` | `true` → the per-turn LLM deep-extract (`src/daemon.c:8154`) is skipped; **0** `turn:deep_extract` relations exist |
| Regex fallback (`daemon_comfort_summary.c`) | **1** relation written, ever |
| Relations by provenance | 453/526 one-off backfill (`chat.db:*`), 72 legacy NULL, 1 comfort_summary |
| Contact entities | 134 across **10** contacts; **2** created in the last 30 days |
| Entity types | 468/572 `UNKNOWN`; `TOPIC` exists (enum) but 1 entity uses it |
| Lowercase multi-word "names" | 308, of which 273 under `self` (backfill LLM prompt) |
| Grounding recall on 40 real moments (2026-09-27) | 0/40 lexical; matched-length specificity 45% of Seth's |

Additional code facts: `hu_graph_upsert_entity` never changes an existing
entity's type (`graph.c:354`); entity `provenance`/`confidence` columns exist
but no writer sets them; the JSONL importer (`graph_ingest.c:176`) has no type
field and always creates objects `UNKNOWN`.

## 2. Goal and success criteria

Each contact's graph holds the **named** people, places, orgs and events they
talk about, typed and fresh, so grounding can name them.

- **E2E recall (primary):** on 40 recent real inbound 1:1 moments, the number
  whose grounding block contains ≥1 typed name rises from the measured
  baseline (re-measured by the new harness before any write) to **≥15/40**
  under `HU_GRAPH_NAMES=live`. *Amended:* the flip needs the harness's paired
  `flip_gate_met` (prod parameters and gates, live ≥15/40, `typed_lost == 0`,
  `lexical_lost == 0`, `typed_gained ≥ 1`, live > the `--baseline` run's live),
  because OFF renders the same type suffixes and the live fallback fills misses
  in both modes, so "live ≥15 with OFF unchanged" cannot isolate the reader.
- **Safety:** with `HU_GRAPH_NAMES=off` the grounding output is byte-identical
  to today for identical graph contents (pinned by test).
- **No daytime GPU load:** the per-turn catcher uses no model.
- Specificity at matched length is re-measured after LIVE (diagnostic, not a
  gate for this change).

## 3. Decisions (user, 2026-09-29)

1. **Both**: a zero-model per-turn catcher (immediate) + a nightly typed LLM
   pass in the curator run (thorough, authoritative).
2. **Retype, keep everything**: existing UNKNOWN entities are typed (names →
   PERSON/PLACE/ORG/EVENT, phrases → TOPIC). Nothing is deleted; `graph.db` is
   backed up first.
3. Ships OFF→SHADOW→LIVE per `.claude/rules/feature-gate-requires-measurement.md`.

## 4. Components

### 4.1 Graph API — typed upsert with a retype policy (`src/memory/graph.c`)

- `hu_graph_upsert_entity_typed(g, contact, contact_len, name, name_len, type,
  provenance, confidence, &id)`: insert sets `type`, `provenance`,
  `confidence`; on an existing row it bumps `last_seen`/`mention_count` like
  today and changes `type` only when the pure predicate allows it.
- `bool hu_graph_entity_retype_allowed(hu_entity_type_t old, hu_entity_type_t
  new)` (public, truth-table tested): allowed iff `old ∈ {UNKNOWN, TOPIC}` and
  `new ∈ {PERSON, PLACE, ORGANIZATION, EVENT, TOPIC}` and `old != new`, or
  `old == UNKNOWN` → any non-UNKNOWN. A name type is never downgraded;
  EMOTION is never touched.
- Provenance is written only when the row has none (first writer wins).

### 4.2 Per-turn name catcher (`src/memory/name_extract.c`, pure)

`hu_name_extract(text, len, known, known_count, out, out_cap)` returns up to
`out_cap` candidates `{name, kind}`, `kind ∈ {KNOWN, CAPITALIZED}`:

- **KNOWN**: an existing entity name of this contact (typed or Capitalized
  UNKNOWN; not TOPIC/EMOTION) appears in the text, case-insensitive,
  word-boundary (`~/.claude/rules/substring-classifier-pitfalls.md`).
- **CAPITALIZED**: a run of 1–3 tokens each starting uppercase followed by a
  lowercase letter, not at a sentence start, not in the stoplist (I, I'm, days,
  months, greetings "Hey/Hi/Ok/Lol/Yeah/Thanks", "God", "Mom"/"Dad" stay
  allowed), 2–40 chars.

Daemon wiring (`src/daemon/daemon_name_catch.c`, sibling call next to
`hu_daemon_store_conversation_summary` at `daemon.c:8255`, same guard, runs in
`llm_decides` mode): input is the **contact's inbound text only** (never the
generated reply — no self-reinforcing hallucination). Gate
`HU_NAME_CATCH=off|shadow|live` via `hu_gate_mode_from_env`, default OFF:

- SHADOW: log `name_catch shadow: known=%zu new=%zu (not written)`.
- LIVE: KNOWN → `hu_graph_upsert_entity` bump (freshness); CAPITALIZED new →
  `hu_graph_upsert_entity_typed(..., UNKNOWN, "names:turn", 0.3)`. One-shot
  info log when disabled (`.claude/rules/silent-config-gated-subsystems.md`).

### 4.3 Importer — typed entity lines (`src/memory/graph_ingest.c`)

The JSONL contract gains an optional line kind; fact lines are unchanged:

```json
{"kind":"entity","contact":"+15550000001","name":"Salim","type":"person","source":"names:nightly","confidence":0.8}
```

`type ∈ {person, place, org, event, topic}` (anything else → line skipped).
Entity lines go through `hu_graph_upsert_entity_typed`. The CLI prints
`{"imported":N,"entities":E,"skipped":M,...}`; success iff `N+E > 0`.

### 4.4 Nightly typed pass (`scripts/insight_stream.py --names`)

- For each eligible 1:1 contact (same enumeration, `curator_never.json`,
  suppressions, `--deadline` as the wide pass) with inbound or outbound
  messages in the last `--names-days` (default 2): one GLM call on :8741
  (thinking suppressed) with a numbered transcript and a names-only prompt
  returning `[{name, type, evidence:[n]}]`, types person/place/org/event/topic.
- **Verification**: a name is kept only if it appears (case-insensitive,
  word-boundary) in the text of a cited message; else counted
  `names_rejected`. Contact's own display name and Seth's names are dropped.
- Output: a 0600 JSONL of entity lines (`source:"names:nightly"`,
  `contact` = the chat.db handle as E.164, matching graph `contact_id`), fed to
  `human memory import-facts` (`--human-bin`, default
  `~/.local/bin/human-daemon`). Without `--write` it stops before import.
- Manifest: counts only (`contacts`, `names_kept`, `names_rejected`,
  `by_type`, `import_entities`, `model_errors`); exit codes match the wide
  pass (2 refused/nothing written, non-zero when every contact errored).
- Scheduled in the existing `ai.human.insight-nightly` chain as
  `--names --deadline 07:30 --write`, **before** the wide pass and joined with
  `;` on both sides. The wide pass checks its deadline only between contacts,
  so it ends at or after 07:30 whenever contacts remain; a step after it would
  find the window closed. With `;`, a `--names` refusal never blocks the wide
  pass, and the wide pass's use of the deadline never starves `--names`.
- A `--write` import that times out (the importer commits per row) reports that
  the graph may be partially updated, never "nothing written".

### 4.5 One-time migration (`scripts/graph_retype_entities.py`)

- Backs up `graph.db` with the SQLite online backup API to
  `~/.human/backups/graph.db.bak-retype-<ts>` (0600) before any write; refuses
  (exit 2) if the backup fails or the daemon's graph is locked.
- Batches the existing `UNKNOWN` entities (40 per call) to GLM for typing
  (same five types; each answer must be one of the batch's names), then emits
  entity lines (`source:"names:migrate"`) through the importer. Unanswered
  names stay UNKNOWN. `--dry-run` prints counts only. Idempotent.

### 4.6 Reader gate (`src/agent/graph_grounding.c`)

`HU_GRAPH_NAMES=off|shadow|live` (default OFF), parsed with
`hu_gate_mode_from_env`, applied inside the non-agent composition so the probe
and the live turn share it:

- OFF: today's behaviour, byte-identical.
- LIVE: entity lines are drawn only from PERSON/PLACE/ORGANIZATION/EVENT and
  Capitalized UNKNOWN; typed names get a ranking bonus over UNKNOWN; TOPIC
  entities are rendered on one separate line `Been talking about: a, b, c`
  (max 3, most recent); EMOTION stays excluded from the fallback.
- SHADOW: compose both, inject the OFF block, log
  `names shadow: off=%zu live=%zu bytes names=%zu (not injected)`.

A refactor extracts the lexical→fallback→self composition out of
`hu_agent_load_graph_grounding` into
`hu_graph_ground_compose_turn(loader, contact, msg, flags, &ctx, &len, &stats)`
(no agent dependency) so `human memory ground --full` measures the real path.

### 4.7 Measurement harness (`scripts/eval_name_grounding.py`)

Samples the last 40 inbound 1:1 messages (14 days, ≤8 per contact) from
chat.db read-only; copies graph.db to a 0600 temp file; runs
`human memory ground --full <contact> <text>` under `HU_GRAPH_NAMES=off` and
`=live` against the copy; reports counts only (non-empty blocks, blocks with
≥1 typed name, distinct typed names, bytes). Refuses (exit 2, writes nothing)
with <40 moments available. Output `~/.human/logs/name-grounding-<ts>.json`.

## 5. Error handling

- Every write path goes through the C importer or the typed upsert; Python
  never writes graph.db except the migration's backup copy.
- The catcher never blocks a reply: errors are logged and swallowed, like the
  comfort summary.
- The nightly pass isolates each contact (one model error ≠ run failure) and
  honours `--deadline` between contacts.
- The migration aborts before writing if the backup fails; restore is
  `cp <backup> ~/.human/graph.db` with the daemon stopped.

## 6. Rollout (after merge; amended 2026-09-30)

1. Build-prod + `scripts/install-human-daemon.sh`; `scripts/verify-deploy.sh`.
   Plist: `HU_NAME_CATCH=off`, `HU_GRAPH_NAMES=shadow` (catcher OFF at deploy).
   Env changes take effect only after bootout + bootstrap of the service.
2. Baseline: `eval_name_grounding.py` before any new write; keep the result.
3. Migration `graph_retype_entities.py --write` (verified backup first).
4. One manual `insight_stream.py --names --write` pass (no `--deadline`).
5. Plist: `HU_NAME_CATCH=live`; reload; confirm the one-shot
   `name_catch active` log line.
6. Re-run the harness with `--baseline <step 2 result>`. Flip
   `HU_GRAPH_NAMES=live` iff it reports `flip_gate_met: true` (and with Seth's
   go-ahead: it changes messages sent as Seth). If the only blocker is
   `no_lexical_lost`, stay SHADOW and report the paired counts to Seth
   (relevance against names is his call). Any other blocker: stay SHADOW.
7. Add `--names --deadline 07:30 --write` to the nightly chain before the wide
   pass, joined with `;` (§4.4); check the next morning's
   `names-manifest-YYYYMMDD.json`.

## 7. Testing

- C: `tests/test_name_extract.c` (truth table incl. sentence start, stoplist,
  word boundary, lowercase known names), `tests/test_graph.c` additions
  (retype predicate truth table, typed upsert upgrade/no-downgrade,
  provenance first-writer), `tests/test_graph_ingest.c` / `test_cli_memory_import.c`
  (entity lines, bad type skipped, success iff N+E>0),
  `tests/test_graph_grounding.c` (OFF byte-identical, LIVE excludes TOPIC from
  entity lines + renders the topic line, typed bonus, SHADOW injects OFF).
  All sqlite-gated tests follow `.claude/rules/test-source-gate-symmetry.md`.
- Python: `tests/test_insight_stream_names.py`, `tests/test_graph_retype.py`,
  `tests/test_eval_name_grounding.py` — hermetic (fake model, temp sqlite,
  fake `human` binary), added to the existing CI pytest step.

## 8. Out of scope

Re-enabling the per-turn LLM deep-extract; fixing `backfill_facts.py`'s
`self` attribution; entity alias/merge ("Sal" vs "Salim"); the curator's
`HU_INSIGHT_GRAPH_MIRROR` step C (superseded by 4.4 for names).
