---
title: Named entities — gates, nightly pass, migration and measurement
created: 2026-09-29
status: operator-facing
spec: docs/superpowers/specs/2026-09-29-named-entity-extraction-design.md
---

# Named entities

How each contact's graph learns the named people, places, organizations and
events they talk about, and how grounding names them. Design:
[spec](../superpowers/specs/2026-09-29-named-entity-extraction-design.md);
build: [plan](../superpowers/plans/2026-09-29-named-entity-extraction.md).

Three independent switches control the feature. All three start off, and each
one moves forward only on a measurement.

## Gate 1: `HU_NAME_CATCH` (per-turn catcher, daemon plist)

| Value | Effect |
|---|---|
| `off` (default) | No work. Logs once per process: `name_catch disabled (HU_NAME_CATCH unset or off) …`. |
| `shadow` | Reads each contact's raw inbound text (1:1 only, never the generated reply, no model) and logs `name_catch shadow: known=N new=M (not written)`. |
| `live` | A known name bumps its entity. A new Capitalized name becomes an UNKNOWN entity with provenance `names:turn`, confidence 0.3. |

It writes the graph but does not change what is sent, so it can go `live`
first.

## Gate 2: `HU_GRAPH_NAMES` (grounding, daemon plist)

| Value | Effect |
|---|---|
| `off` (default) | Grounding is composed exactly as before. |
| `shadow` | Composes both ways, logs `names shadow: off=… live=… bytes names=… (not injected)`, and injects the `off` block. |
| `live` | Seeds only typed names (person, place, organization, event) and Capitalized UNKNOWN names, gives typed names a +0.5 bonus, and moves topics to one `Been talking about:` line. |

`live` changes the messages sent as Seth. It needs the harness result below
**and** Seth's go-ahead (a blind A/B, or an explicit override).

## Gate 3: the nightly `--names` step

```bash
python3 scripts/insight_stream.py --names --deadline 07:30 --write
```

For every eligible 1:1 contact with texts in the last `--names-days` (default
2), the local model lists the names in their texts. A name is kept only if a
cited human text contains it. Kept names are imported as `names:nightly` entity
lines (confidence 0.8) through `human memory import-facts`. Without `--write`
nothing is imported, and the kept names stay in the 0600 dry-run file under
`~/.human/names/`. The counts-only manifest is
`~/.human/logs/names-manifest-YYYYMMDD[-dryrun].json`.

The step is off until it is appended to the `ai.human.insight-nightly` chain
(rollout step 6).

## One-time migration

```bash
python3 scripts/graph_retype_entities.py --dry-run   # counts only: no model, no backup, no import
python3 scripts/graph_retype_entities.py --write     # probe -> classify -> lock probe -> backup -> import
```

The local model types each contact's UNKNOWN names in batches of 40. The
answers are written as retype-only lines (`names:migrate`, 0.6) that never
create a row, never bump recency, and never downgrade a name type. Unanswered
names stay UNKNOWN, and a re-run only sees what is still UNKNOWN.

`--write` refuses (exit 2) when:

- the `--human-bin` importer cannot import entity lines (probed on a throwaway
  temp graph);
- another connection holds the graph.db write lock for more than 1 s;
- the backup fails its checks.

The backup is taken with the SQLite online backup API right before the only
write. It goes to `~/.human/backups/graph.db.bak-retype-<ts>` (0600) and is
checked with `integrity_check` and an equal entity count.

**Restore.** This discards every daemon write made to graph.db after the backup
was taken:

```bash
launchctl bootout gui/$(id -u)/ai.human.service-loop   # KeepAlive restarts a kill
lsof ~/.human/graph.db                                 # must print nothing
rm -f ~/.human/graph.db-wal ~/.human/graph.db-shm      # before the copy
cp <backup> ~/.human/graph.db && chmod 600 ~/.human/graph.db
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.service-loop.plist
```

## Measurement: `eval_name_grounding.py`

```bash
python3 scripts/eval_name_grounding.py
```

The harness samples the last 40 inbound 1:1 texts from the last 14 days, at most
8 per contact. It skips group chats, Seth's own messages, reactions, short
codes and the daemon's loopback handle (`channels.imessage.loopback_handle` in
`~/.human/config.json`). For each text it runs the real composition,
`human memory ground --full <contact> <text>`, twice: once with
`HU_GRAPH_NAMES=off` and once with `=live`.

- Both modes run against the **same** private copy of graph.db. The copy is
  0600, taken with the SQLite backup API from a read-only connection, and
  deleted afterwards, also on failure. The live graph.db is never opened for
  writing.
- The other gates default to prod (`HU_GRAPH_GROUNDING`,
  `HU_GRAPH_GROUNDING_CONTACT_FALLBACK` and `HU_GRAPH_GROUNDING_SELF_FACTS` all
  `live`) and are recorded in the output under `gates`. Change them with
  `--grounding`, `--fallback` and `--self-facts`.
- Each text is passed as one argv element (no shell). It is never written to
  the output, stdout or stderr.
- A block counts as a typed-name block when the probe header's `names=` is
  above 0. `names=` counts only the contact's block. The owner's
  `About you:` block never counts, because an owner fact is not the contact's
  name.

The output is `~/.human/logs/name-grounding-<UTC YYYYmmdd-HHMMSS>.json` (0600).
It holds counts only: `n`, `days`, `per_contact_cap`, `gates`, and per mode
`nonempty_blocks`, `typed_name_blocks`, `distinct_typed_names`, `bytes_total`.
It also holds `target_n` (40), `target_live_typed_name_blocks` (15) and
`live_gate_met`.

**The LIVE gate:** `live_gate_met` is true only when `n` is exactly 40 and
`modes.live.typed_name_blocks >= 15`. A run with another `--n` never meets it.

**Refusals.** The harness exits 2 and writes nothing when:

- the human binary is missing or not executable;
- chat.db or config.json cannot be read;
- fewer than 40 moments exist;
- the graph copy fails or has no `entities` table;
- any probe exits non-zero, times out, or prints output that breaks the
  `--full` contract. A daemon config with no memory backend prints
  `Memory backend: none` instead of a header, and that counts as a failure.

A partial run is not a measurement, so it never writes a result.

**What the `off` column is not.** It measures `off` on the same graph snapshot
as `live`. It does **not** show that `off` is byte-identical to the code before
this change. That is proven by the C golden tests in
`tests/test_graph_grounding.c` (`test_compose_turn_golden_and_stats`,
`test_names_off_and_shadow_leave_the_golden_unchanged`). The harness also does
not compare its `off` column to an earlier run, because the graph changes
between runs.

## Rollout (spec §6)

1. Build-prod, run `scripts/install-human-daemon.sh`, then
   `scripts/verify-deploy.sh <commit>`.
2. Plist: `HU_NAME_CATCH=live`, `HU_GRAPH_NAMES=shadow`.
3. Baseline: run `eval_name_grounding.py` before any new write.
4. Migration `--write`, then one manual `insight_stream.py --names --write`.
5. Re-run the harness. It must report `live_gate_met: true`.
6. Append `&& /opt/homebrew/bin/python3 scripts/insight_stream.py --names --deadline 07:30 --write`
   to `ai.human.insight-nightly`. Back up the plist first, then bootout and
   bootstrap it.
7. Set `HU_GRAPH_NAMES=live` only with the harness result and Seth's go-ahead.
