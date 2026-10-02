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
| `scripts/blind_ab/replay_export_turns.py` | chat.db (read-only) → `~/blind_ab_run/<name>/turns.jsonl`; drops turns the daemon answered |
| `scripts/blind_ab/replay_driver.py snapshot` | copies the daemon state into the run dir without touching the source |
| `scripts/blind_ab/replay_driver.py run` | turn by turn: a pristine time-cut state per turn, one sandboxed `human replay` process per arm |
| `human replay` (`src/app/cli_replay.c`) | turns in, result rows out (the driver gives it one turn at a time) |
| `src/daemon/replay_turn.c` | one turn through the production functions |
| `scripts/blind_ab/replay_feed.py` | rows → blind A/B triples and sheets + stats per arm vs Seth |

## Guarantees, and what enforces them

| Guarantee | Enforcement | Test |
|---|---|---|
| Nothing is sent | The turn runs on a null channel. Every outbound entry (send, send_event, react, reply, react_emoji, send_sticker) only counts the call. The CLI aborts if a count is ever non-zero. | `replay_turn_never_calls_channel_send` (mutation-checked), `replay_channel_counts_every_outbound_entry` |
| Nothing leaves the machine | `hu_replay_provider_create_local` refuses any endpoint that is not `127.0.0.1`, `localhost` or `[::1]`, and `--provider` must be a local server type (`mlx_local`, `mlx-local`, `mlx_http`, `mlx-http`, `compatible`, `llamacpp`, `lmstudio`, `ollama`). That provider is the only one the agent and the director use. The local embedder replaces the Gemini one, and semantic recall must embed on loopback. Every libcurl proxy variable points at the dead port `127.0.0.1:9`, with `NO_PROXY` covering loopback. The update check is skipped. On macOS the driver also runs every turn under `sandbox-exec`, which denies any non-loopback network connection. | `replay_url_loopback_accepts_only_loopback_hosts`, `replay_provider_create_local_refuses_cloud_endpoint`, `cli_replay_provider_allowlist_is_local_only`, `test_run_refuses_a_cloud_endpoint`, `test_sandbox_profile_confines_writes_and_network` |
| Nothing outside the run dir is written | Four layers. (1) Each turn's process gets `HOME`, `HU_STATE_DIR` and `TMPDIR` inside its own scratch dir, so even a path built from `$HOME` lands there. (2) `human replay` refuses to start unless `HU_STATE_DIR` and `HU_MEMORY_SQLITE_PATH` name a snapshot outside the live state dir. (3) The turn detaches the agent's session store. (4) On macOS, `sandbox-exec` denies every write outside the turn's scratch dir. The guard-rejection training logs (`m3-dpo-rejections-*`, `m3-rewrite-pairs`) used to be built from `$HOME`; they now go through `hu_paths_state` and land in the scratch state. The snapshot byte-copies each db and its WAL and backs up the copy; it never opens the source, because even a `mode=ro` connection creates `-shm`/`-wal` files next to it. | `test_real_replay_writes_only_inside_the_run_dir[off,auto]` (real binary: nothing outside the run dir changes, the rejection log lands in scratch), `test_sandbox_blocks_writes_outside_the_turn_dir` + its no-sandbox control, `dpo_log_path_follows_state_dir_not_home`, `cli_replay_isolation_refuses_the_live_state`, `replay_turn_detaches_the_session_store`, `test_snapshot_*` |
| Seth's replies are Seth's | The exporter drops any turn whose response the daemon produced: a reply bubble matching an `outbound_sends` row or a session-store assistant row for that contact within 10 minutes (text containment either way), or a tapback within 10 minutes of any daemon output for that contact. It prints only the count dropped. | `test_drops_turns_the_daemon_answered`, `test_record_outside_the_window_does_not_exclude`, `test_daemon_activity_drops_a_tapback_turn`, `test_refuses_without_provenance_db` |
| chat.db is read-only, and only counts are printed | `mode=ro` URI. Output is written 0600 into a 0700 run dir, which must be outside the repo. | `test_chat_db_untouched_and_output_private`, `test_refuses_a_run_dir_inside_the_repo` |
| Arms are comparable | Every arm replays a turn from the same pristine, time-cut state, turn-major, so drift on the model server hits both arms alike. The wrapper provider pins the model and (with `--temperature`) the temperature on every call, the director's included. It fingerprints each reply request (`reply_fp`), and the shaping seed is fixed. The same arm run twice gives byte-identical requests. | `test_each_turn_sees_only_its_past_and_no_other_turns_writes`, `replay_turn_gate_env_changes_reply_request`, `replay_provider_pins_model_and_temperature` |
| A partial arm is not a measurement | The driver marks an arm INCOMPLETE (exit 1) on a missing or malformed row, an error row or a non-zero exit; a malformed row is counted and skipped, never raised. The feed refuses an arm the manifest marks INCOMPLETE, a missing manifest, and arms that cover different turn sets, and writes nothing. `--allow-partial` compares only the shared turns, with a warning. | `test_run_marks_a_malformed_row_incomplete_without_raising`, `test_feed_refuses_an_incomplete_arm_from_the_manifest`, `test_feed_refuses_arms_covering_different_turns`, `test_feed_refuses_without_a_manifest` |

`$HOME`-built paths reachable from the reply path (audited 2026-10-02,
`grep -rn 'getenv("HOME")' src`): the only one that **wrote** was
`hu_response_guard_log_dpo_negative` (from `agent_stream.c`, `agent_turn.c` and
`init_proposer.c`), now routed through `hu_paths_state`. The rest only read
(AddressBook contact photos, gcloud ADC, the Photos library, the mlx venv path)
or use `$HOME` only as a guard in front of `hu_paths_state`/`hu_paths_chatdb`
(`m3_rewrite_capture.c`, `persona_crypt.c`, `data/loader.c`, `auto_profile.c`,
`skill_write.c`). With `HOME` set to the scratch dir, those reads find nothing,
so contact photos, the chat.db auto-profile and AddressBook birthdays are not
replayed (they would read today's data anyway).

## Time fidelity: each turn sees only its past

Before each turn, the driver clones the snapshot copy-on-write and deletes every
row written at or after the turn's timestamp from these tables (rows with an
unknown time are kept):

| db | tables (column) |
|---|---|
| memory.db | `memories`, `embeddings`, `messages` (`created_at`, text); `contact_insights` (`created_at_ms`); `episodes`, `commitments`, `prospective_memories`, `emotional_moments`, `inside_jokes`, `micro_moments`, `contact_mood_log`, `growth_milestones` (`created_at`); `temporal_events` (`extracted_at`); `mood_log` (`set_at`); `pattern_observations` (`observed_at`); `outbound_sends` (`sent_at_ms`) |
| graph.db | `entities`, `relations` (`first_seen`); `community_summaries` (`generated_at`); `negative_memory`, `hyperedges` (`created_at`) |

The manifest records the rows deleted, any table absent, and any table the
filter could not touch (an error leaves that table unfiltered, and it is
reported, not hidden). Not covered:

- rows created earlier but **updated** later (an `updated_at` past the turn keeps its newer content);
- `memories_vec` embeddings of deleted memories, which stay as orphans (semantic recall joins back to `memories`);
- cognition.db, the persona files and config, which are today's;
- **the clock.** Production code reads wall time directly (`time(NULL)`, `hu_time_wall_ms`) and has no time source to override, so the prompt's date, time-of-day overlays and "N hours since" hints describe replay time, not the turn's. Every arm shares this, so arm deltas stay valid; absolute comparisons with Seth carry it.

Each turn of each arm starts from a fresh clone of that time-cut state, so
nothing one turn writes (memories, insights, the session store) reaches the next
turn or the other arm.

## Fidelity: what is replayed and what is not

Replayed, by calling the production function, in the daemon's order:

- the director through `hu_daemon_director_decide`, the seam the daemon calls too, with `HU_DIRECTOR_FORMS` situation text and the unknown-event guard. Director v2 (PR #590, `HU_DIRECTOR_V2`) must hook this seam rather than daemon.c, or the harness will not see it.
- the silence override and the tapback decision
- slice A (`hu_daemon_reactive_context_load`): contact profile, style notes, channel history (served from the turn's own thread) and cross-channel context
- slice B (`hu_daemon_reactive_prompt_build`): this is where `HU_THREAD_CONTEXT` lives
- length calibration (`hu_daemon_append_length_calibration`, moved out of daemon.c by this change)
- the reply budget through `hu_daemon_reply_budget` (channel cap, relational ratio, brief cap for media), the seam the daemon calls too. Length policy (PR #580, `HU_LENGTH_POLICY`) is inline in its daemon.c today; when it lands it must move into this helper and into `hu_daemon_append_length_calibration`, or the harness will not see it.
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
- **Future knowledge** beyond what the time filter removes (see Time fidelity). The exporter picks the newest turns first (`--since-days 14`) to keep that gap small.

## The 40-turn run, comparing two arms

Run these from the repo root of a build that has `human replay` (this branch or later).
They use `:8741` (production, light use: strictly sequential, one turn every 3 s;
each turn also starts a fresh process, a second or two) unless `--endpoint`
points at a spare server. `--model` defaults to the config's
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
#    (the exporter prints how many turns it dropped because the daemon answered them)
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
- `state/base/`, the snapshot; `work/` holds the turn being replayed (with `--keep-state`, every turn's scratch dir as `work/<arm>-<turn id>`)
- `out/<arm>.jsonl`, one row per turn: `action` (text, tapback, silence, dropped or error), `bubbles`, `bubble_count`, `director_*`, `voice_memo`, `retried`, `ai_tell`, `max_chars`, `reply_fp`, `reply_system_bytes`, `channel_outbound_calls` (always 0)
- `logs/replay.log`, the binary's own log for every turn and arm, which may quote text
- `requests/` (with `--dump-requests`)
- `manifest.json`
- `feed/`

`feed/stats.json` has the same fields for Seth's real replies to the same turns
and for each arm. **Seth's `silence_share` is always 0 by construction**: the
exporter only emits turns Seth responded to (a text or a tapback), so his
denominator never contains a silence. An arm's silence share above 0 is the
director leaving a turn Seth answered on read.

| Field | Meaning |
|---|---|
| `tapback_share`, `silence_share`, `dropped_share` | fraction of turns |
| `len_mean` / `len_median` / `len_p90`, `bubbles_mean` | characters per text reply (all bubbles joined with `\n`); p90 is nearest-rank: the ⌈0.9·n⌉-th smallest |
| `ks_len_vs_seth` (`ks_D`, `ks_p`) | two-sample Kolmogorov–Smirnov on reply lengths, arm vs Seth: D, and the asymptotic p (Kolmogorov series with the Stephens small-n correction). Lengths are integers, so ties make p conservative. |
| `question_rate` | text replies containing `?` |
| `fragment_rate` | text replies with a bubble cut off mid-clause, defined below |
| `deflection_rate`, `deflection_n` | among text replies to an inbound containing `?` (that count is `deflection_n`), the share that do not answer, defined below |
| `same_request_as_<first arm>` / `compared_with_<first arm>` | turns whose reply request was byte-identical to the baseline arm's, out of the turns where both made one |

The heuristics, exactly (`scripts/blind_ab/replay_feed.py`, pinned by `test_replay_feed.py`):

- **Fragment.** A bubble is a fragment when, after trailing whitespace, it ends in `,` `:` `;` `-` `–` `—`; or when its last character is not terminal punctuation (`. ! ? … ) " ' ” ’ *`) and not a code point above U+2000 (emoji and symbols count as an ending), AND its last word is in `DANGLING`: `a an the and but or because cause cuz if than to my your our their his her its i just very gonna gotta lock`. A final `to` after `up used want have need got going supposed able hope plan try` is complete ("what are you up to"). Auxiliaries and stranded prepositions are deliberately absent ("yeah it is", "who are you with").
- **Deflection.** Defined only when the inbound asked (`?`). The reply *answers* when its first word is in `ANSWER_OPENERS` (`yes yeah yea yep yup ya ye no nah nope sure ok okay k kk def definitely absolutely totally of course probably prob already tonight today tomorrow tmrw now soon later`), or it contains a number or clock time (`7`, `7:30`, `7pm`, `noon`), or it shares a content word with the inbound. A content word is 4 or more letters and not in the stop list. A reply that does not answer is a deflection when it contains a stock non-answer phrase, matched on word boundaries (`idk`, `i dont know`, `not sure`, `no idea`, `we'll see`, `hard to say`, `depends`, `let me think`, `let me check`, `get back to you`, `maybe later`, `whatever you think`, `up to you`, …), or when it is only a question back. So "yeah what time?" answers; "not sure, we'll see" does not.

These are heuristics for spotting regressions. Promotion still rests on the
judged and human-rated sheets (`docs/evaluation/`), not on these rates.

## Rollback

The harness is offline tooling. Nothing in the daemon reads its output, and
`human replay` runs only when invoked. The daemon.c changes in the same PR
are moves with no behaviour change (`hu_daemon_outbound_sanitize`,
`hu_daemon_append_length_calibration`, `hu_daemon_reply_budget`,
`hu_daemon_director_decide`, `hu_daemon_director_silence_overridden`,
`hu_daemon_reactive_turn_end`), with three small exceptions:

- the all-reasoning fallback now checks its buffer size before writing;
- a missed-message acknowledgement in front of an emptied reply now sends just the acknowledgement, not the acknowledgement plus a blank line (`hu_daemon_join_ack`);
- guard-rejection logs follow `HU_STATE_DIR`. They still land in `~/.human/training-data` for the daemon, which does not set it.

To roll back, revert the PR.
