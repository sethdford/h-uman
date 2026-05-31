# Seth-Aliveness — Design

## Components

- **`hu_personal_model_build_prompt_for_contact`** (new) — `src/memory/personal_model.c`
  + `include/human/memory/personal_model.h`. Contact-scoped variant of
  `hu_personal_model_build_prompt`. When `contact_handle != NULL`, the
  emotional/anticipatory/causal walk (today `personal_model.c:800-870`) filters to
  that one handle. The existing `hu_personal_model_build_prompt(model, buf, cap)`
  becomes a thin delegate passing `contact_handle = NULL` → broadcast preserved.
  Serves **AC-B1**.

- **`hu_init_proposer` context population** — `src/agent/init_proposer.c:189-193`.
  Replace the zeroed `PERSONAL_MODEL / MEMORY / AWARENESS` slots with real data
  rendered into proposer-owned scratch buffers on the output struct. Reuses the
  new contact-scoped builder (the proposer already picks a target contact), an
  `agent->memory` recent-item summary, and a formatted local time-of-day string.
  Serves **AC-A1**.

- **Temporal-decay default** — `src/agent/memory_loader.c:167`. Flip the hardcoded
  `temporal_decay_factor = 0.0` to a conservative config-sourced default; decay is
  already applied at `src/memory/retrieval/engine.c:62-65` when the factor is `>0`.
  Serves **AC-B2**.

- **Threaded scheduled send** — `src/context/conversation.c` (`sched_queue` record +
  `hu_conversation_schedule_message_on` + `hu_conversation_flush_scheduled_for`),
  `include/human/context/conversation.h`, and the daemon flush call site
  (`src/daemon.c:1523`). Adds an optional `parent_guid` to the scheduled record so
  proactive/follow-up sends thread. Serves **AC-C1**.

- **Register-aware send time** — `src/agent/follow_up.c:39` (`hu_followup_compute_send_time`)
  + `include/human/follow_up.h` (`hu_followup_input_t`). Adds a banter signal so a
  short/casual message gets a fast send time while substantive messages keep the
  warmth/chronotype schedule. Serves **AC-C2**.

## Data flow

### AC-B1 — contact-scoped reply prompt
1. Inbound iMessage → `autoresponder.c:476` reply flow, `contact_handle` in scope.
2. Today line 493 calls `hu_personal_model_build_prompt(model, persona_buf, cap)` →
   broadcasts all 8 contacts' emotional context into `persona_buf`.
3. **New**: call `hu_personal_model_build_prompt_for_contact(model, contact_handle,
   persona_buf, cap)` → only the in-scope contact's context lands in the prompt.
4. Non-contact callers (anywhere passing no handle) keep calling the old delegate →
   unchanged broadcast.

### AC-A1 — grounded proposal
1. Daemon initiative tick → `hu_init_proposer_tick_with_provider` (`daemon.c:14539`).
2. Proposer assembles its context fields. **New**: for the chosen target contact,
   render `build_prompt_for_contact` into a scratch buffer, set
   `content/bytes[HU_INIT_FIELD_PERSONAL_MODEL]`; summarize `agent->memory` into
   `[MEMORY]`; format local time-of-day into `[AWARENESS]`.
3. Empty personal_model/memory → zero bytes written → output byte-identical to today.

### AC-B2 — recency-weighted retrieval
1. `hu_memory_loader_load` (`agent_turn.c:1475`) builds retrieval options.
2. **New**: `temporal_decay_factor` sourced from config (default conservative nonzero).
3. `engine.c:62` applies `hu_temporal_decay_score` per result; recent memories rise.
4. Config `0.0` (or absent → conservative default) — the `0.0` path reproduces today.

### AC-C1 — threaded scheduled send
1. Follow-up watcher detects a read-but-unreplied outbound; it knows that message's
   guid. **New**: schedule via the threaded variant carrying `parent_guid`.
2. `sched_queue` record stores `parent_guid` (new fixed field).
3. `hu_conversation_flush_scheduled_for` returns it via a new out-param.
4. Daemon flush (`daemon.c:1523`): if `parent_guid` present and `vtable->reply`
   exists → threaded reply; else today's flat `vtable->send(..., NULL, 0)`.

### AC-C2 — banter speed
1. Follow-up/scheduled path builds `hu_followup_input_t`. **New**: set the banter
   signal from the incoming message register (length + punctuation heuristic, or an
   existing shape signal if cheaply available).
2. `hu_followup_compute_send_time`: banter → scaled-down base delay (still jittered,
   still chronotype-snapped, still 24h-capped); non-banter → today's schedule.
3. Designated-initializer call sites that don't set the new field get `0`/false →
   substantive path → byte-identical.

## Decisions

- **New `_for_contact` variant over a new param on `build_prompt`** (AC-B1): matches
  the existing `load_for_contact`/`ingest_for_contact` naming, avoids touching the
  `_with_overlay` variant and every existing caller, and makes the safe default
  (NULL = broadcast) explicit. Chosen over editing the signature, which would force
  every call site to opt in and risk a missed site silently changing behavior.
- **Config-sourced decay default, conservative value** (AC-B2): chosen over a bare
  magic constant so the value is tunable and the `0.0` escape hatch is a first-class
  config, not a code edit. Conservative magnitude limits ranking churn; the
  "0.0 reproduces today" test pins the rollback path.
- **Additive `_threaded` schedule variant + new fixed `parent_guid` field** (AC-C1):
  chosen over reusing the inbound threading path because scheduled sends are async
  and decoupled from the live reply; the parent guid must be persisted in the queue
  record. Old `schedule_message_on` delegates with `NULL` parent → existing callers
  (main.c:463, conversation.c:8099) unchanged. Most invasive AC; isolated to its own
  task + worktree.
- **Banter signal as a struct field on `hu_followup_input_t`** (AC-C2): chosen over a
  separate wrapper function because `compute_send_time` is already the single timing
  predicate; a `0`-default field keeps the pure-predicate property and leaves every
  existing designated-initializer call site byte-identical.
- **Reuse the contact-scoped builder inside the proposer** (AC-A1): the proposer
  already selects a target contact, so feeding it the same per-contact context the
  reply path uses keeps proactive and reactive grounding consistent — and lets AC-A1
  ride on AC-B1's new function rather than duplicating a walk.

## Risks

- **Decay regresses retrieval quality** (AC-B2) — a too-aggressive factor could bury
  durable facts. Mitigation: conservative default, config-tunable, and the
  `0.0`-reproduces-today contract test as a one-line rollback.
- **Scheduled-struct change ripples** (AC-C1) — `sched_queue` is a static fixed array;
  adding a field + a flush out-param touches the conversation API and the daemon flush
  site. Mitigation: additive variant, NULL-safe flush out-param, full-suite run; the
  existing scheduled-send outbound-sanitizer path (daemon.c:~1510) is preserved.
- **Banter misclassification** (AC-C2) — a substantive short message could be rushed.
  Mitigation: heuristic biased toward "substantive unless clearly casual"; quiet-hours
  snapping still bounds the fast path so nothing fires at 3am.
- **Cross-contact leak fix changes prompt content** (AC-B1) — tone/recall could shift
  for multi-contact models. This is the intended improvement; pinned by a test that
  asserts X-in / Y-out, plus the NULL-broadcast byte-identical test for safety.
- **Peer-session collision** — all five live in distinct files from the peer's
  `scripts/` work; each phase runs in its own worktree, merged when the peer is quiet.

## Test strategy (per AC, positive-contract)

- **AC-A1**: populated model ⇒ nonzero `bytes[PERSONAL_MODEL]` + `[AWARENESS]`;
  empty model ⇒ byte-identical output (pinned).
- **AC-B1**: model with X+Y facts, build for X ⇒ X's emotional context present, Y's
  absent; build with NULL contact ⇒ byte-identical broadcast (pinned).
- **AC-B2**: two equal-salience memories, recent ranks above old after decay; factor
  `0.0` ⇒ identical ordering to today (pinned).
- **AC-C1**: scheduled record with parent guid ⇒ flush returns it, daemon threads;
  no parent ⇒ flat send (no regression).
- **AC-C2**: one-word casual ⇒ markedly shorter send time than multi-sentence
  substantive to same contact; quiet-hours snap applies to both; unset field ⇒
  today's schedule (pinned).
