---
title: Director v2 — intent instead of length, decisions from Seth's own data (HU_DIRECTOR_V2)
created: 2026-10-02
status: operator-facing
---

# Director v2

The scene director runs before every reactive reply. It decides whether Seth
texts, reacts with a tapback, or stays silent, and it writes a one-line
`direction` that the reply model follows.

- v1 lives in `src/daemon/daemon_director.c`.
- v2 lives in `src/daemon/director_v2.c` (prompt, call and gate) and
  `src/daemon/director_tapback.c` (Seth's learned behaviour).
- The daemon calls v2 at one site, `hu_director_v2_decide` in `src/daemon.c`.

## Why

The prod log from 2026-09-17 to 2026-10-01 holds 456 director decisions.

- **335 of the 456 (73.5%)** carried a length or deflection cue. Examples are
  "one line", "a few words", "keep it light", "non-committal" and "don't
  over-explain". This count comes from `hu_director_directive_flags`, the
  measurement helper below.
- **108 of the 456 (23.7%)** were tapback-only.
- Three real cases:
  - A parent shared covid news and asked how Seth was doing. The direction
    was "Acknowledge the sickness briefly … one line".
  - A walk-me-through request was directed to "Keep it light and
    non-committal", and the reply dodged.
  - "Seth your AI is messed up?" got "Laugh it off" and a "Liked" tapback.

v1 sees 5 messages. It has no contact context, and its prompt says "BREVITY
IS THE DEFAULT".

## What v2 changes

There are no fixed behaviour rules. Delay, silence and tapbacks are judged
from the thread and from Seth's measured behaviour, never from ranges or word
lists.

1. **A schema, not examples.** The direction has three parts: what they really
   mean or ask; the move (engage fully, ask a follow-up, share something of
   his own, or just react); and what to draw on from the shared history.
   - The prompt never mentions length, which a test pins.
   - It keeps three principles:
     - answer a real question or a walk-me-through request; never dodge;
     - never invent events, people or outcomes;
     - never fabricate a memory when tested.
   - There are no examples to copy. A test pins that too.
2. **Seth's measured behaviour as plain facts.** One line from the
   learned-style profile, for example:
   > How Seth replies, measured from his own texts: he usually answers them
   > after about 4 minutes; with them, when they ask something he replies with
   > only a reaction 3% of the time (n=40); his reactions: heart 70%, haha 30%.

   When there is no data, the line is absent and the model judges from the
   thread. There are no default ranges.
3. **More context.**
   - The last 12 messages, labelled Seth/Them. They come from a separate read,
     so the rest of the turn still sees its usual 10.
   - A `Contact:` line with the relationship and Dunbar layer, never a name.
   - The facts line above.
   - When fields are at their maximum, the oldest history messages are dropped
     first, so the new message and the `This turn:` line always fit. A test
     pins this.
4. **Prompt size, measured.**
   - The system prompt is 1,372 bytes, or 1,811 bytes with the
     `HU_DIRECTOR_FORMS` block.
   - The forms block lists the vocabulary plus one principle: use each form
     "as Seth would with this person". It has no who-gets-a-GIF list, no
     sad-news rule and no daily cap.
     - The model gets the contact's relationship, plus Seth's learned
       `voice_memo_rate`, `gif_rate` and `share_rate` when the profile has
       them (optional; absent fields are omitted).
     - The static safety limits stay downstream in
       `src/daemon/daemon_expressive.c` (`hu_expressive_*_allowed`) and are
       not restated in the prompt.
   - The per-turn context is capped at 2,560 bytes.
   - The worst case is therefore about **4.4 KB**.

## Tapback-only: learned data, no rules

The data comes from `<persona dir>/<persona>.learned-style.json`, schema
`learned-style/v1` (learner #584). These **optional** fields are read from the
contact's `shape:<x>` bucket, then the contact's `overall`, then `global`:

- `tapback_only_rate`
- `tapback_types`
- `tapback_disengage_rate` with `tapback_disengage_n`
- `latency_p50_s` (already in v1)

The shape is the learner's bucket rule: `question`, `story` or `casual`.

**The post-check is a learned threshold.** It turns a tapback-only choice into
text only when Seth's rate for this contact and shape is at or below the
**n-weighted lower quartile of his own cells at the same level**:

| Lookup answered at | Cutoff built from | Minimum n per cell |
|---|---|---|
| contact × shape bucket | every contact's `shape:*` buckets | 3 (learner `MIN_BUCKET_N`) |
| contact `overall` | every contact's `overall` | 5 (learner `MIN_CONTACT_N`) |
| `global` | no peer cells | no override |

How the cutoff behaves:

- The minimum n values are the learner's own thresholds in
  `scripts/learned_style_profile.py` (#584). They are not new constants.
- `overall` and the buckets are never mixed in one distribution.
- Fewer than 4 cells means no quartile, so there is no override.
- **All zeros:** if every peer cell is 0, Seth never answers with only a
  tapback, so the cutoff is 0 and every tapback-only choice is overridden.
  That is what his data says, and a test pins it.
- An override changes only the form (text instead of tapback). The model's own
  delay and direction stand; there are no canned values.
- With no data (`tapback_src=nodata`), the model's choice stands.

**What exists today.**

- learned-style/v1 has **no reaction fields yet**, so until the learner writes
  them, every turn logs `tapback_src=nodata`. `latency_p50_s` already exists
  and reaches the prompt.
- **No disengagement data exists in h-uman's DB:**
  - `outbound_sends.kind` admits only text, media and reply;
  - `reaction_lookup` holds their reactions to us;
  - `proactive_decisions` is proactive-only.

  The learner should derive `tapback_disengage_rate` from chat.db: Seth's
  tapbacks, then what the contact sent next.
- **Reconciliation with #586** (unmerged):
  - `hu_director_inbound_shape` mirrors `hu_learned_style_shape_inbound`
    exactly, with the same injected-note prefixes. Its tests are #586's own
    vectors.
  - The profile reader has an mtime + size cache.
  - When #586 merges, delete both and call its functions.

## The gate

`HU_DIRECTOR_V2=off|shadow|live`, default **off**, parsed by
`hu_gate_mode_from_env`.

- **off:** exactly `hu_daemon_director_call`. A test pins that the result is
  byte-identical and that no v2 call is made.
- **shadow:** v1 decides and returns at once. v2 **never sits on the reply
  path**.
  - It runs on a detached worker, one job in flight at a time. While one is in
    flight, the turn logs `v2=skipped_busy`.
  - The worker does the 12-message read, the profile read and the director
    call, then logs one aggregate line (enums and counts only):

        [director_v2 shadow] v1_action=text v2_action=text v1_brevity=1 v2_brevity=0 tapback_overridden=0 tapback_src=nodata shape=question v2_bytes=3120 v2=ran

  - A test holds the provider for 3 s, and decide still returns v1 in under
    1 s.
  - **Its own provider.** The worker never calls the shared director provider
    (`g_classify_provider`) or `agent->provider`. Both may sit behind the
    non-reentrant `reliable` wrapper and a Gemini fallback.
    - It builds a plain compatible provider on the primary's endpoint, with no
      wrapper and no fallback (`hu_director_v2_worker_provider_create`). The
      endpoint is `default_provider`, or `reliability.primary_provider` when
      that is `reliable`, which is `mlx_local` in prod.
    - It is created lazily, rebuilt if the endpoint moves, and used only by the
      worker. `hu_director_v2_shutdown` frees it when the daemon loop ends.
    - A test pins that the shared and agent providers see zero calls.
  - **Privacy.** Shadow runs v2 only when that endpoint is **loopback**,
    decided by real host parsing in `hu_compatible_url_is_loopback`.
    - The scheme is required, and userinfo and port are stripped.
    - The host must be exactly 127/8 (strict octets), `localhost` or `[::1]`.
    - So `http://127.0.0.1:8080@evil.com/` is not local.
    - Otherwise the turn logs `v2=skipped_nonlocal` and the thread goes
      nowhere.
  - **Shutdown.** The daemon's teardown calls
    `hu_director_v2_shutdown(5000)`. It refuses new jobs and waits for the
    running worker before its inputs go away. If the worker outlives the wait,
    its provider is kept rather than freed under it.
- **live:** v2 decides inline, replacing v1's call, and falls back to v1 if v2
  fails. The log line is
  `[director_v2 live] v2_action=… v2_brevity=… tapback_overridden=…
  tapback_src=… shape=… v2_bytes=… fallback_v1=0|1`.

**Cost:** SHADOW adds no reply latency, only background director calls on the
local primary model (`:8741` in prod), one at a time. LIVE costs one director call, as today, plus a chat.db read and a
cached profile lookup.

## Promotion: SHADOW → LIVE

Prerequisite: shadow lines say `v2=ran`. This needs a loopback primary, as in
prod. Run SHADOW for at least 7 days, then measure:

**(a) Shadow log.**

```bash
grep -h '\[director_v2 shadow\].*v2=ran' ~/.human/logs/service-loop-error.log | awk '
  {for(i=1;i<=NF;i++){split($i,kv,"="); f[kv[1]]=kv[2]}
   n++; v1b+=f["v1_brevity"]; v2b+=f["v2_brevity"]
   if (f["shape"]!="casual") {qs++; if (f["v2_action"]=="tapback") qt++}
   if (f["tapback_src"]!="nodata") data++}
  END{printf "n=%d v1_brevity=%.1f%% v2_brevity=%.1f%% qs_tapback=%d/%d learned_data=%d/%d\n",
      n,100*v1b/n,100*v2b/n,qt,qs,data,n}'
```

Pass when all of these hold:

- `v1_brevity` is near the 73% baseline.
- `v2_brevity` is **≤ 25%**.
- On question and story turns, v2's tapback share is no higher than Seth's
  learned rate for those shapes.
- `learned_data` covers most turns. Without the reaction fields, only the
  length half is measured.

**(b) Replay A/B.** Use `human replay` from the sibling PR
`feat/replay-harness`. Reply depth must rise, and the fragment rate must not.

**(c) Blind gate.** Run `scripts/blind_ab_gate.py` with `HU_DIRECTOR_V2=live`
on the candidate arm. Detection must hold or fall.

**Gated on:** do not flip to LIVE without all three.

## Rollback

Remove `HU_DIRECTOR_V2` from the service-loop plist environment, or set it to
`off`. Then reinstall with `scripts/install-human-daemon.sh`. v2 persists
nothing.
