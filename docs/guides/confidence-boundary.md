---
title: Confidence Boundary (HU_CONFIDENCE_BOUNDARY)
created: 2026-10-02
status: operator-facing
---

# Confidence Boundary (`HU_CONFIDENCE_BOUNDARY`)

`HU_CONFIDENCE_BOUNDARY=off|shadow|live` (default `off`) keeps what one
contact told the twin out of its conversations with anyone else. People slip
and gossip; the twin must not. Contract:
`include/human/memory/confidence_boundary.h`. Code:
`src/memory/confidence_boundary.c` (rules, backstop),
`src/memory/confidence_filters.c` (per-path filters),
`src/memory/repos/confidence_repo_sqlite.c` (provenance columns).

## Which recall paths could carry another contact's items

Audited 2026-10-02 against `main` at `72c2668f8`.

| Path | Scope before this change | Crossed contacts? | Under the gate |
| --- | --- | --- | --- |
| Semantic recall (`memory_loader.c`, hybrid engine or v1 recall) | Rows owned by another contact dropped since `339fab354` (`keep_contact_scope`). Global rows (no session) reach every contact. | **Yes.** In prod, 812 of 825 `memories` rows are global. 751 are `experience:` rows that store each inbound message verbatim (`Task: <message>`). There are also 40 commitments, 3 `agent-promise:<contact>` rows and 7 misc rows. Only the 11 `_pref:` rows are the owner's. | Filtered |
| Episodic `## Recent Sessions` (`daemon.c` → `hu_episodic_load`) | None. It recalls `_ep:` with an empty session, so every contact's session summary goes into every reply. | **Yes** | Filtered (`hu_episodic_load_for_contact`) |
| Commitments block (`hu_commitment_store_list_active`) | Session-owned rows are scoped. Global rows (saved with no session) reach everyone. | **Yes**, via global rows | Filtered |
| Personal model "Key facts" and the per-contact walk (`personal_model.c`) | None. One model holds facts from every contact. The walk renders "X recently mentioned…" for **every** handle in the model, by design (Sprint B.2). | **Yes** | Filtered (`hu_confidence_pm_view`) |
| Graph grounding (`graph_grounding.c`) | `entities`/`relations` WHERE `contact_id = ?`. The owner block reads only the `self` scope (imported owner corpora). | No | Not needed |
| Contact insights and curator wide rows | `contact_insights WHERE contact_id = ?1`. Wide rows cite only that contact's chat and Seth's own messages. | No | Not needed |
| Wiki head (`hu_wiki_page_read`) | One page per contact | No | Not needed |
| STM | `hu_stm_clear` before each contact batch (`daemon.c`) | No | Not needed |
| Prospective memory | `contact_id = ? OR contact_id IS NULL`. Prod has 0 rows with no contact. | No (today) | Not needed |
| W12 planner / world model | Plan steps stamped with `contact_id` | Not found; not exhaustively verified | Not covered |

Not covered, and known: personal-model **topics and goals** carry no
provenance. A topic another contact raised can still appear as a bare topic
word. `agent_stream.c` (the streaming path) also renders the personal model,
but the daemon reply path uses `hu_agent_turn`, which is covered.

## Provenance

`memories` gains `source_contact TEXT` and `share_level INTEGER`:
`1` = private_to_source, `2` = shareable, `3` = owner_self. Columns are added
and backfilled when the engine opens. Every engine store re-stamps its row at
write time. These columns are metadata: with the gate off, nothing reads them.

Write-time rules (`hu_confidence_derive_row`). The first match wins:

1. Key `_pref:` → owner_self.
2. The row has a session → private to that contact.
3. Key `agent-promise:<c>:`, `contact:<c>:` or `_ep:<c>` → private to `<c>`.
4. A session-less write made during contact X's turn (`experience:`,
   global commitments) → private to X. The writer calls
   `hu_confidence_stamp_write` with `memory->current_session_id`. The same
   write with no contact active (the owner's CLI) → owner_self.
5. The source channel is an owner channel (`cli`, `stdin`, `human`,
   `tool:human`, `self_email`, `calendar_self`) → owner_self.
6. Anything else → **private, source unknown**. Unknown rows are excluded
   from every contact's conversation.

Backfill applies the same rules without rule 4. So the 751 existing
`experience:` rows and the 40 global commitments become
private/unknown, and the 11 `_pref:` rows become owner_self. Nothing is
classified shareable automatically. No LLM classifier runs: write-time
derivation is deterministic. The owner marks a row shareable during review
(below). A local-model classifier could relax rows to shareable later; it
could never make a row less private.

Personal-model facts (`hu_confidence_derive_fact`) follow the same pattern. A
fact with a stamped handle is private to that handle. A fact with no handle
from an owner channel is owner_self. Anything else is private/unknown, which
covers `agent_turn`'s ingest of an inbound message, stamped with no handle.

The rule (`hu_confidence_excludes`): in a conversation with contact X, drop an
item that is private (or unstamped) when its source is unknown or is not X.
Owner_self and shareable items always pass. With no current contact, nothing
is dropped.

## What each state does

- **off**: no filtering, counting or logging. Prompts are byte for byte what
  they were. `tests/test_confidence_boundary.c` pins OFF == the unscoped
  episodic loader, and OFF == SHADOW for the semantic, episodic, commitment
  and personal-model paths.
- **shadow**: each path logs one counts-only line per event, then sends
  what OFF sends. The line has counts, no text and no contact:
  `[confidence-boundary shadow] path=<semantic|episodic|pm_facts|commitments> considered=N would_exclude=M`
  and, once per reply,
  `[confidence-boundary shadow] path=outbound ledger=N sentences_would_drop=M`.
- **live**: the items are left out of the prompt. The **outbound backstop**
  (`hu_confidence_backstop_apply`, called after the hallucination guard in
  `agent_turn.c`) then drops any draft sentence that still names an excluded
  item. A sentence matches when it contains one of the item's Capitalized
  names (word-bounded, case-insensitive) **and** one other content word of 4+
  letters from that item. If every sentence matches, the reply is empty and
  nothing is sent. Silence is the safe failure.

## Measurement: SHADOW to LIVE

1. **Run SHADOW for 7 days.** Set `HU_CONFIDENCE_BOUNDARY=shadow` in the
   service-loop plist (rollback commands below, with `shadow`). Aggregate:

   ```bash
   grep -o '\[confidence-boundary shadow\] path=[a-z_]* considered=[0-9]* would_exclude=[0-9]*' \
     ~/.human/logs/service-loop-error.log |
   awk '{split($3,p,"=");split($4,c,"=");split($5,w,"=");
         n[p[2]]++; C[p[2]]+=c[2]; W[p[2]]+=w[2]}
        END{for(k in n) printf "%-12s events=%d considered=%d would_exclude=%d\n",k,n[k],C[k],W[k]}'
   grep -c 'path=outbound.*sentences_would_drop=[1-9]' ~/.human/logs/service-loop-error.log
   ```

   - **Enough traffic:** at least 50 `semantic` events. With fewer, keep
     SHADOW running; there is no verdict.
   - **Expect `semantic` exclusions to be high at first.** About 798 of the
     825 prod rows backfill as private/unknown. That is the leak this gate
     closes, not noise.

2. **Owner review of about 20 would-be-excluded items.** Seth runs this
   himself: it prints his own data to his own terminal, and no session should
   run it for him.

   ```bash
   sqlite3 -readonly ~/.human/memory.db "SELECT key, substr(content,1,100)
     FROM memories WHERE share_level = 1 AND COALESCE(session_id,'') = ''
     ORDER BY random() LIMIT 20;"
   ```

   Mark each one: **private** (correct to keep out), **owner** (Seth's own
   fact) or **shareable** (public or general).
   - **Promote** if at most 2 of 20 are owner/shareable and none of those
     2 is something the twin needs in order to reply well.
   - **Fix the rows, not the rule:**
     `UPDATE memories SET share_level = 3 WHERE key = '…'` (owner) or `= 2`
     (shareable). Work on a copy, or with the daemon stopped.
   - If more than 2 of 20 are wrong, the derivation rules need another
     owner-side writer added to rule 4 or 5 before going LIVE.

3. **Backstop false-positive check.** Text is never logged, so measure the
   rate. `sentences_would_drop >= 1` should be rare: under 2% of replies. A
   higher rate means the name+word match is too loose for real traffic.
   Investigate with a local replay, never by logging message text.

4. **LIVE canary for 7 days**, then the blind A/B human gate. LIVE removes
   context, so replies to a contact should not lose anything that contact
   told the twin itself. The 2026-07-27 baseline is detection 0.225, CI
   [0.123, 0.350]. Keep LIVE only if detection does not rise past the CI's
   upper bound.

## Rollback

```bash
/usr/libexec/PlistBuddy -c "Set :EnvironmentVariables:HU_CONFIDENCE_BOUNDARY off" \
  ~/Library/LaunchAgents/ai.human.service-loop.plist
launchctl bootout gui/$(id -u)/ai.human.service-loop
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.service-loop.plist
```

If the key is absent, use `Add :EnvironmentVariables:HU_CONFIDENCE_BOUNDARY string off`.
`off` restores today's prompts exactly. The two provenance columns stay in
`memories`. Nothing reads them with the gate off, and they need no undo.
