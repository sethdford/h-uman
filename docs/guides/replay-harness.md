---
title: Real-turn replay harness — measure a gate on real turns before LIVE
created: 2026-10-02
status: operator-facing
---

# Real-turn replay harness

Every behaviour gate (thread context, immersive context, length policy,
learned style, voice triggers, director v2) needs a measurement before it goes
LIVE. The older blind-A/B generator (`scripts/blind_ab/gen_huuman_replies.py` →
`human eval run`) builds its prompt with `hu_persona_build_prompt_compact` and
never reaches `hu_agent_turn`, `hu_prompt_build_system` or the daemon's reactive
path, so two gate arms come out byte-identical. Waiting for shadow logs costs
about a week per promotion.

The replay harness takes real inbound iMessage turns and runs each through the
daemon's `llm_decides` reply path, offline, once per arm. It writes the text
that would have been sent, the bubbles, and what the director decided. Nothing
is sent.

| Piece | What it does |
|---|---|
| `scripts/blind_ab/replay_export_turns.py` | chat.db (read-only) → `~/blind_ab_run/<name>/turns.jsonl` |
| `scripts/blind_ab/replay_driver.py snapshot` | copies the daemon state into the run dir without touching the source |
| `scripts/blind_ab/replay_driver.py run` | one `human replay` process per arm, one after another |
| `human replay` (`src/app/cli_replay.c`) | one arm: turns in, result rows out |
| `src/daemon/replay_turn.c` | one turn through the production functions |
| `scripts/blind_ab/replay_feed.py` | rows → blind A/B triples and sheets + stats per arm vs Seth |

## Guarantees, and what enforces them

| Guarantee | Enforcement | Test |
|---|---|---|
| Nothing is sent | The turn runs on a null channel. Every outbound entry (send, send_event, react, reply, react_emoji, send_sticker) only counts the call. The CLI aborts if a count is ever non-zero. | `replay_turn_never_calls_channel_send` (mutation-checked), `replay_channel_counts_every_outbound_entry` |
| Nothing leaves the machine | `hu_replay_provider_create_local` refuses any endpoint that is not `127.0.0.1`, `localhost` or `[::1]`. That provider is the only one the agent and the director use. The local embedder replaces the Gemini one, and semantic recall must embed on loopback. Every libcurl proxy variable points at the dead port `127.0.0.1:9`, with `NO_PROXY` covering loopback, so any other host fails. The update check is skipped. | `replay_url_loopback_accepts_only_loopback_hosts`, `replay_provider_create_local_refuses_cloud_endpoint`, `test_run_refuses_a_cloud_endpoint` |
| `~/.human` is never written | `human replay` refuses to start unless `HU_STATE_DIR` and `HU_MEMORY_SQLITE_PATH` name a snapshot outside `~/.human`. The turn detaches the agent's session store. The snapshot byte-copies the db and its WAL and backs up the copy; it never opens the source, because even a `mode=ro` connection creates `-shm`/`-wal` files next to it. Each arm starts from a fresh copy. | `cli_replay_isolation_refuses_the_live_state`, `replay_turn_detaches_the_session_store`, `test_snapshot_*` |
| chat.db is read-only, and only counts are printed | `mode=ro` URI. Output is written 0600 into a 0700 run dir, which must be outside the repo. | `test_chat_db_untouched_and_output_private`, `test_refuses_a_run_dir_inside_the_repo` |
| Arms are comparable | The wrapper provider pins the model and (with `--temperature`) the temperature on every call, the director's calls included. It fingerprints each reply request (`reply_fp`), and the shaping seed is fixed. The same arm run twice gives byte-identical requests. | `replay_turn_gate_env_changes_reply_request`, `replay_provider_pins_model_and_temperature` |
| A partial arm is not a measurement | The driver marks an arm INCOMPLETE (exit 1) on a short output, an error row or a non-zero exit. The feed refuses such arms and writes nothing. | `test_run_marks_a_short_arm_incomplete`, `test_feed_refuses_an_arm_with_errors` |

## Fidelity: what is replayed and what is not

Replayed, by calling the production function, in the daemon's order:

- the director (`hu_daemon_director_call`), with `HU_DIRECTOR_FORMS` situation text and the unknown-event guard
- the silence override and the tapback decision
- slice A (`hu_daemon_reactive_context_load`): contact profile, style notes, channel history (served from the turn's own thread) and cross-channel context
- slice B (`hu_daemon_reactive_prompt_build`): this is where `HU_THREAD_CONTEXT` lives
- length calibration (`hu_daemon_append_length_calibration`, moved out of daemon.c by this change)
- the relational, brief and media reply budget
- voice-first (`HU_VOICE_FIRST`, `HU_VOICE_TRIGGERS_V2`: `voice_memo`/`voice_reason` in each row)
- the G6 director arm and end-turn
- `hu_agent_turn_stream_v2` with the tool registry emptied, as on the live path; immersive context, learned style and the lean head run inside this call
- the AI-tell retry and the quality retry (with the kept draft)
- the validator chain and `hu_daemon_shape_text_inplace`
- typos (seeded)
- `hu_daemon_outbound_sanitize` (moved out of daemon.c by this change)
- plaintext-before-split, then choreography or `hu_conversation_split_response` plus iMessage cadence

Not replayed. All of these are constant across arms, so arm deltas stay valid, but absolute numbers against Seth carry the gap:

- **The inline daemon.c context blocks between slice B and the turn**: the trust directive, replay insights, honesty check, anti-repetition, relationship-tier calibration, link share, mood, time-of-day overlay, situational injections, crisis directive and group hint. A gate whose call site sits in one of these blocks is invisible to the harness. The sensitivity check below catches that. The fix is to move the block into a shared helper, as this change did for calibration.
- **The director's model.** Production's director is the Gemini Flash-Lite classify provider. The harness, being local-only, runs the director on the replay endpoint. Director decisions (tapback, silence) therefore differ from production's, so compare director arms with each other, not with production's tapback share.
- **Timing and delivery**: delays, read receipts, the typo follow-up correction, the missed-message acknowledgement, voice synthesis and effects.
- **Session-store history.** Prior turns come from the thread before the inbound message, not from the daemon's session table.
- **Group chats**: the exporter emits 1:1 turns only.
- **The legacy non-`llm_decides` path.** `--no-director` (or `HU_REPLAY_DIRECTOR=off`) skips the director, but the reply path stays the `llm_decides` one.
- **Future knowledge.** The memory snapshot is taken now, so it knows things that happened after a past turn. The exporter picks the newest turns first (`--since-days 14`) to keep that gap small.

## The 40-turn run, comparing two arms

Run these from the repo root of a build that has `human replay` (this branch or later).
They use `:8741` (production, light use: strictly sequential, one turn every 3 s)
unless `--endpoint` points at a spare server. `--model` defaults to the config's
`default_model`. Pass the adapter-bearing name the server expects if it differs.

```bash
RUN=replay-$(date +%Y%m%d)-thread
PLIST=~/Library/LaunchAgents/ai.human.service-loop.plist   # production's gate env

# 0. A build with `human replay`
cmake --preset dev && nice -n 10 cmake --build build --target human -j8

# 1. 40 recent 1:1 turns (read-only on chat.db; prints counts only)
python3 scripts/blind_ab/replay_export_turns.py --name "$RUN" --limit 40 --since-days 14

# 2. Snapshot config, personas, contacts and memory/graph/cognition dbs into the run dir
python3 scripts/blind_ab/replay_driver.py snapshot --name "$RUN"

# 3. Sensitivity check, 2 turns: the arm must change the reply request
python3 scripts/blind_ab/replay_driver.py run --name "$RUN" --limit 2 --temperature 0 \
    --dump-requests --base-env-plist "$PLIST" \
    --arm prod: --arm thread:HU_THREAD_CONTEXT=live
python3 scripts/blind_ab/replay_feed.py --name "$RUN"
#    "WARNING: arm thread sent byte-identical reply requests..." => the gate never
#    reached the prompt; stop and find its call site. To see the difference:
#    diff ~/blind_ab_run/$RUN/requests/t0001.prod.txt ~/blind_ab_run/$RUN/requests/t0001.thread.txt

# 4. The full run (about 15-25 min: 2 arms x 40 turns x director + reply)
python3 scripts/blind_ab/replay_driver.py run --name "$RUN" --temperature 0 --delay-ms 3000 \
    --base-env-plist "$PLIST" --arm prod: --arm thread:HU_THREAD_CONTEXT=live

# 5. Triples, blind sheets and automatic stats vs Seth
python3 scripts/blind_ab/replay_feed.py --name "$RUN" --sheets

# 6. Local judge (spare server, NOT :8741 under load), then score, per arm
for ARM in prod thread; do
  D=~/blind_ab_run/$RUN/feed/$ARM
  python3 scripts/blind_ab/synthetic_judge.py "$D/rating_sheet.csv" --out "$D/judged.csv" \
      --endpoint http://127.0.0.1:8745/v1/chat/completions --model <judge-model>
  python3 scripts/blind_ab/score.py "$D/judged.csv" --key "$D/answer_key.json" \
      --json-out "$D/score.json"
done
```

Other arms follow the same pattern: `--arm len:HU_LENGTH_POLICY=live`,
`--arm style:HU_LEARNED_STYLE=live`, `--arm voice:HU_VOICE_TRIGGERS_V2=live`,
`--arm imm:HU_IMMERSIVE_CONTEXT=live`. Several gates in one arm:
`--arm both:HU_THREAD_CONTEXT=live,HU_LENGTH_POLICY=live`. Without
`--base-env-plist`, every arm starts with no `HU_*` variables at all; the
parent shell's are never inherited.

## Reading the output

`~/blind_ab_run/<name>/` (0700) holds:

- `turns.jsonl`
- `state/`, the snapshot plus one fresh copy per arm
- `out/<arm>.jsonl`, one row per turn: `action` (text, tapback, silence, dropped or error), `bubbles`, `bubble_count`, `director_*`, `voice_memo`, `retried`, `ai_tell`, `max_chars`, `reply_fp`, `reply_system_bytes`, `channel_outbound_calls` (always 0)
- `logs/<arm>.log`, the binary's own log, which may quote text
- `requests/` (with `--dump-requests`)
- `manifest.json`
- `feed/`

`feed/stats.json` has the same fields for Seth's real replies to the same turns
and for each arm:

| Field | Meaning |
|---|---|
| `tapback_share`, `silence_share`, `dropped_share` | fraction of turns |
| `len_mean` / `len_median` / `len_p90`, `bubbles_mean` | text replies only |
| `question_rate` | text replies containing `?` |
| `fragment_rate` | text replies with a bubble cut off mid-clause: no terminal punctuation or emoji, and ending on a word nothing ends on (`a`, `the`, `and`, `to` other than "up to"/"have to"/…, `my`, `i`, `just`, `gonna`, `lock`, …) or on `,` `:` `-`. Stranded prepositions ("who are you with") and bare auxiliaries ("yeah it is") are complete, so they do not count. |
| `deflection_rate` | text replies that are a stock non-answer, matched on word boundaries (`idk`, `not sure`, `we'll see`, `let me check`, …), or a direct question answered only with a question that shares none of its content words |
| `same_request_as_<first arm>` / `compared_with_<first arm>` | turns whose reply request was byte-identical to the baseline arm's |

These are heuristics for spotting regressions. Promotion still rests on the
judged and human-rated sheets (`docs/evaluation/`), not on these rates.

## Rollback

The harness is offline tooling. Nothing in the daemon reads its output, and
`human replay` runs only when invoked. The daemon.c changes in the same PR
are moves with no behaviour change (`hu_daemon_outbound_sanitize`,
`hu_daemon_append_length_calibration`, `hu_daemon_director_silence_overridden`).
The one difference is that the all-reasoning fallback now checks its buffer
size before writing. To roll back, revert the PR.
