# Emotional register — a measured axis, local, judge-pinned

**Status:** measurement shipped, prompt rule gated OFF (2026-09-06).

## Substantive register — rule 15, measured and A/B'd (2026-09-13)

The register finding below (substantive scenarios end judged AI, casual
ones do not) got the same treatment as distress: a judge-free card axis
and a rule that states the measurement. `scripts/reply_pairs.py` is the
shared reader for both axes (inbound matching a predicate → Seth's next
in-chat reply within 30 min). `substantive_reply` = inbound ≥150 chars or
a real question ≥60 chars (small talk excluded); 120 d, n=63: median 27
chars, 73% under 60, median 1 sentence, answer-first 0.33. Rule 15 renders
from those numbers behind `HU_SUBSTANTIVE_REGISTER` (same ladder as 14);
`HU_PERSONA_RULES_BUF` grew to 3072 because 14+15 live overflowed 2048,
and the builder now drops 15, then 14, rather than dropping every rule.

The gating measurement is the harness change made for it: the nightly's
multi-turn stage now runs the production prompt with 3 repeats per
scenario and scores by last-third mean (majority vote for the hard-AI
verdict). Off vs live on the three substantive scenarios, 3 repeats each,
sampling fixed (see the correction above), Gemini voice judge:

| scenario | off: first→last (hard-AI) | live: first→last (hard-AI) | opinion off/live | flow off/live |
|---|---|---|---|---|
| debate_opinions | 5.7→5.7 (2/3) | 5.0→7.7 (1/3) | 4/9 | 3/9 |
| news_reaction_chain | 7.3→8.3 (0/3) | 4.3→9.0 (0/3) | 8/7 | 8/9 |
| advice_seeking | 7.0→4.7 (3/3) | 7.3→6.0 (2/3) | 3/8 | 5/6 |
| **last-third mean, all repeats (n=9)** | **6.22, hard-AI 5/9** | **7.56, hard-AI 3/9** | | |

Direction right on every scenario and on the pooled mean (+1.3), so the
gate goes to SHADOW in the plist. Not LIVE: this is a synthetic judge, and
the ladder's LIVE step needs a human-judged round.

What the judge still flags, and the next lever: the remaining AI tell in
every arm is structural — "[short affirmation] — [brief comment]" on
nearly every turn. Rule 15 names that shape and says "no em-dash", and the
live arm produced MORE of it: last-third replies containing an em-dash
went 62% → 96% (15/24 → 23/24). A negated pattern in the prompt primes
the pattern. The next variant should describe the positive shape (one
clause, or two sentences) and not mention the dash at all; re-run the
same 3-repeat A/B before touching the gate again. Rule 15 text lives in
`hu_style_card_render_substantive_rule` (src/persona/style_card.c).

### Rule 15 v2 + dash strip, and the noise floor (2026-09-13 PM)

Seth's own typed texts carry an em-dash in 0 of 954 (chat.db; the 4 hits
in 958 are h-uman rating prompts sent to himself). Two levers landed in
b6c07c3fa: rule 15 v2 states the shape positively and never names the
dash or a "never"; the outbound strip stage maps U+2014/U+2013 plus their
spaces to ", " (dropped when leading/trailing, never doubling punctuation),
and the style card gains `dash_rate`. Same 3-repeat A/B, plus a re-run of
the off arm to measure the noise between identical prompts:

| run | last-third mean (n=9) | hard-AI | last-third replies with an em-dash |
|---|---|---|---|
| off, AM | 6.22 | 5/9 | 15/24 (62%) |
| live v1 (names the dash) | 7.56 | 3/9 | 23/24 (96%) |
| off, PM re-run | 5.11 | 7/9 | 24/24 (100%) |
| live v2 (positive shape) | 5.56 | 5/9 | 8/24 (33%) |

Read it in two halves. **Judge-free:** the positive wording cut the dash
share from 100% to 33% against its own off arm (and from 96% under v1) —
the priming finding holds, and the strip stage takes the rest to zero on
the real send path (the harness posts to the server directly, so the strip
is not in this loop). **Judge:** two runs of the identical off prompt sit
1.1 apart, so neither +1.3 (v1) nor +0.45 (v2) clears the floor. Gate stays
SHADOW. A delta under ~1.5 on the nightly's 3-repeat mean is noise; a
decisive read needs more repeats or a paired design where both arms see
the same contact turns.

What the judge names once the dashes are gone: the twin is a "yes-man"
(opinion_strength 2–3 in every live-v2 scenario; debate ends with the twin
accepting "agree to disagree" after agreeing with every point). That is
the second reason in the original finding and rule 15's "take a side" line
does not move it. The next lever is not prompt wording: the opinion-hold
directive (HU_OPINION_HOLD, live) and the persona's own stances are the
place to look for why a stated position never surfaces in these turns.

### The agreement tell: where the stances were supposed to come from (2026-09-13 PM)

Chasing "the twin agrees with everything" through the product path:

- **The A/B never had a stance in the loop.** The multi-turn harness posts
  the `persona show` prompt straight to the server. The daemon's evolved-
  opinions block and the opinion-hold directive (`HU_OPINION_HOLD`, LIVE in
  the plist) are built in the reactive prompt and never reach the harness.
- **Opinion-hold has fired zero times in production, ever** (0 hits across
  all 27 service logs). Its only stance source is `evolved_opinions`, and
  that table held two rows in four months, both harvested by the extractor
  from the twin's OWN replies and both identity slips: "I think I just
  glitched for a second" (2026-05-12) and "I think you might be confusing
  me with someone else" (2026-05-17). Conviction 0.5 clears the 0.4
  injection floor, so every live prompt since May has carried *"On 'I just
  glitched for a': you tentatively believe 'I think I just glitched for a
  second' (shaped by 1 conversations)"* as a position arrived at through
  experience (today's salience log: `kept [evolved_opinion]` at 10:01,
  10:02, 16:36). Fixed: rows purged (backup table
  `evolved_opinions_purged_20260913` + `memory.db.bak-20260913-evolved-opinions-rows`),
  and `hu_evolved_opinion_stance_is_usable` now rejects a topic that opens
  on a pronoun or a sentence carrying an AI/identity slip; the two real
  rows are pinned as rejected in `tests/test_opinions_persistence.c`.
- **The persona has no debatable stances anywhere.** `seth.json` carries a
  values list (nouns) and the prompt says "You have STRONG OPINIONS. Pick a
  side" with nothing to pick. Seth's own texts state a position in 1.5% of
  messages (14/946), and those are logistics ("I think renting for a year
  is ideal"), not hot takes. The prompt asks for a register the corpus does
  not show.
- **What the corpus does show, judge-free:** reflexive agreement as the
  first word. `reply_pairs.agreement_opener_rate` (yeah / exactly / totally
  / 100% / fr / fair…, with bare yes/no/ok excluded as answers):

  | who | agreement-opener rate |
  |---|---|
  | Seth, substantive replies (n=65) | **0.06** |
  | twin, last third, off AM / live v1 | 0.50 / 0.46 |
  | twin, last third, off PM / live v2 | 0.54 / 0.62 |

  A 10× gap, stable across arms, and it is now on the style card
  (`substantive_reply.agreement_opener_rate`) and in every multi-turn
  verdict (`last_third_agreement_opener_rate`, meaned over repeats), so the
  nightly tracks it without the judge.

Next lever, in order: (1) rule 15 v3 states the measured opener fact
("you open on agreement about 1 in 16 times; usually you just say the
thing") — same 3-repeat A/B, read the judge-free rate first, the judge
second; (2) give the harness the product path (the daemon's reactive
prompt builder, or at least the opinion block) so opinion-hold is
measurable at all; (3) only then a real stance source, mined from Seth's
own texts with provenance, never from the twin's output.

## Multi-turn A/B on the production prompt (2026-09-13)

`scripts/eval_multiturn_local.py --persona-prompt production` (new: the
daemon's own prompt with the rules block, gates from the environment) run
once per arm, six 30-turn scenarios, Gemini voice judge, real sampling:

| scenario | off: voice first→last | live: voice first→last |
|---|---|---|
| casual_catchup | 8→7 borderline | 4→9 human |
| emotional_escalation | 3→9 human | 8→7 borderline |
| debate_opinions | 9→3 AI | 6→4 AI |
| banter_humor | 3→8 human | 3→3 AI |
| news_reaction_chain | 4→4 AI | 8→3 AI |
| advice_seeking | 6→4 AI | 3→3 AI |
| **mean last third / hard-AI** | **5.83 / 3** | **4.83 / 4** |

Verdict: no evidence the rule helps over long conversations, and the
measurement cannot say much either way at n=1 per arm: identical prompts
except rule 14 score 8 vs 4 on the *first* third of the same scenario, so
the judge-plus-sampling noise is larger than any effect. What is
consistent across both arms and the week of nightlies: the substantive
scenarios (debate, news reaction, advice) end judged AI; the casual and
emotional ones mostly do not. That is a register finding, not an
emotion-rule finding. Rule 14 stays SHADOW. A decisive multi-turn A/B
needs ≥3 runs per arm and should be scored by last-third mean, not the
pass/fail count.

Also fixed today, from the same transcript: the AI-tell retry hint used to
prescribe four sympathy formulas the model parroted ("damn that's rough");
it now describes the measured register and bans the scaffolds
(66ef31cc4, deployed).

## Direction-aware rule 14 and its first measurement (2026-09-13)

The 2026-09-12 15:18 turn showed the mechanism behind the sympathy gap: to
"😓" the model wrote "I'm sorry to hear that. How can I help you with
this…" and, on retry, "I understand this is frustrating. How can I help
you…" (neither delivered: AI-tell retry, then pre-send abort). Seth's own
replies to sad/frustrated texts (chat.db, 120 d, n=11): median 17 chars,
zero support scaffolds ("Haha, true!", "Answer?", "Yes you can").

That became a measured axis on the card (`distress_reply`, judge-free)
and rule 14 now names the feelings that are there, bans the scaffolds by
name, and states the measured reply length. `scripts/eval_distress_register.py`
is the off/live harness: the 11 real distress inbounds, the product's
prompt (`persona show seth imessage`, rules block now appended), the
serving model on :8741.

| arm | scaffold rate | median chars | sympathy | amusement | neutral |
|---|---|---|---|---|---|
| Seth (n=11) | 0.00 | 17 | | | |
| off | 0.00 | 39 | 0.27 | 0.18 | 0.18 |
| live | 0.00 | 41 | 0.18 | 0.36 | 0.18 |

**Correction, same day, after fixing the server:** the "greedy" server was
a frozen sampler — mlx_lm's `@mx.compile`d sampler captures the main
thread's RNG state and never advances on the admission-queue worker
thread, so every token of every generation drew one fixed key
(gemma-realtime-1 commits 56488f5 + d11a7ec, restarted 03:5x). Re-run
with real samples (4 per inbound, 2–4 distinct each, n=44 per arm):

| arm | scaffold | median chars | sympathy | amusement |
|---|---|---|---|---|
| off | 0.00 | 31 | 0.36 | 0.25 |
| live | 0.00 | 30 | 0.27 | 0.32 |

Same direction and size as the single-draw run. The product path replayed
with the contact's real 12-message history (gateway, no delivery) answers
"damn that's rough" / "you're right, this is a risky situation" — the
sympathetic-validation register, not the support scaffold; the literal
"sorry to hear that" of the 15:18 turn still needs the daemon's memory /
director / retry-hint context to reproduce. Every :8741 number before
2026-09-13 (best-of-N, retries, arena, blind A/B trials, fidelity) was a
single draw.

Caveats that bounded the first run: :8741 was effectively greedy (all 4
"samples" per inbound were byte-identical), so n was 11 distinct outputs per arm;
5 of 11 changed under the rule, all toward Seth's register ("damn
that's rough. been there" → "lol yeah no kidding"); length did not move;
and the bare prompt never produced the support register in either arm,
so the 15:18 failure needs the daemon's full turn context (22-message
history) to reproduce. Verdict: direction right, magnitude small, human
judgement absent → **SHADOW** (renders and logs on the real turn, sends
nothing). LIVE waits on a ≥20-row human preference sheet where the two
arms differ, which needs more distress pairs than chat.db holds today.

## Nightly trend and the recorder cutover (2026-09-12)

| night | n | JSD | twin neutral | intensity | top |
|---|---|---|---|---|---|
| 09-06 | 28 | 0.174 | 0.75 | 0.09 | sympathy |
| 09-07 | 34 | 0.163 | 0.74 | 0.11 | sympathy |
| 09-08 | 39 | 0.138 | 0.72 | 0.11 | sympathy |
| 09-09 | 42 | 0.138 | 0.74 | 0.11 | sympathy |
| 09-10 | 52 | 0.116 | 0.69 | 0.12 | amusement |
| 09-12 | 76 | 0.122 | 0.70 | 0.14 | amusement |

(09-11 deferred: judge down.) Converging toward the card (0.58 neutral,
0.20 intensity, amusement first) with the rule OFF. Keep it OFF.

Found while checking this: `production_outcomes.chosen` was the model's
draft, recorded before the style governor and the send decision — some
rows were never delivered at all. Fixed 2026-09-12 (the daemon records
the delivered text from the send funnel). Verdicts above measure drafts;
emotion labels barely move with casing/punctuation, but do not compare a
pre- and post-cutover window as one series.

## First measurement (2026-09-06, judge GLM-4.5-Air-4bit on :8741)

Seth's card: 60-day window, 964 messages in window, 300 judged, n=299
(1 parse failure). 58% neutral, mean intensity 0.20, top categories
amusement 6%, interest 6%, confusion 4%, joy 3%.

Twin (last 14 days of sent replies, n=28): JSD **0.174** [0.087, 0.188],
gap flagged. The twin is *flatter*, not more enthusiastic: 75% neutral
(+17 pts), intensity 0.09 (−0.11), valence 0.04 (−0.13), and its dominant
feeling is **sympathy** (14% vs ~0% for Seth) where Seth's are amusement
and interest. Rule 14 as written ("stays low-key") would push the wrong
way for this gap; the rendering must become direction-aware (name what is
missing — amusement and interest — not just the ceiling) before any
shadow/live trial. n=28 is thin; three more nightly verdicts first.

## Why

Asked 2026-09-06: can h-uman use Hume's Expression Measurement API, or the
same framework? The API is voice/face only, cloud-hosted, and being wound
down. What transfers is the *framework*: a fixed high-dimensional emotion
taxonomy (Cowen & Keltner 2017, 27 categories), a frozen judge, and a
comparison between distributions — the same shape the style card already
uses for punctuation and casing.

The daemon's replies were already measured on casing, punctuation, length
and emoji. Nobody had measured whether the twin *feels* like Seth: does it
perform enthusiasm he never texts, or sound flat where he is amused?

## What ships

| Piece | Path | Contract |
|---|---|---|
| Shared library | `scripts/emotion_register.py` | taxonomy, judge prompt, lenient label parser, aggregate + bootstrap CIs, Jensen–Shannon divergence, judge-identity check |
| Card writer | `scripts/measure_emotion_card.py` | labels a window of Seth's own outbound iMessages (read-only chat.db, same reader as the style card) with the LOCAL judge on :8741 and writes `~/.human/personas/<persona>.emotion-card.json` (`emotion-card/v1`) |
| Nightly comparison | `scripts/eval_emotion_register.py` | labels the twin's recent sent replies (`production_outcomes`) with the SAME judge and writes JSD + deltas to `~/.human/logs/eval-emotion-register-latest.json`; stage `[4/4]` of `scripts/nightly_eval.sh` |
| C loader | `src/persona/emotion_card.c` | parses the card; `hu_emotion_card_render_rule` renders rule 14; `HU_EMOTION_REGISTER` gate (off default / shadow / live) in `hu_persona_build_absolute_rules_fmt` |
| Tests | `scripts/test_emotion_register.py`, `tests/test_emotion_card.c` | hermetic: fake judge, temp sqlite, temp persona dir |

## Measurement contract (no-number-without-a-measurement)

- **Judge is local.** GLM on :8741 via the OpenAI-compatible endpoint,
  temperature 0. No message text leaves the machine.
- **Judge identity travels with every number.** `judge.id` is
  `<model>|<taxonomy version>|<sha of the prompt template>`. The nightly
  refuses (exit 3, writes nothing) when the serving model or the prompt
  differs from the card's — cross-judge numbers are not comparable.
- **Refusals write nothing.** Below `min_n` (card 100, nightly 20), server
  unreachable (exit 2), or parse failures above 10% → no card, no verdict.
- **Distributions, not a score.** The headline is base-2 Jensen–Shannon
  divergence in [0, 1] between the twin's and Seth's category
  distributions, with a bootstrap CI over the twin sample, plus deltas on
  neutral share, mean intensity and valence. `jsd_max` 0.15 flags a gap;
  it is a provisional threshold and says so in the verdict file.
- **Valence is derived, not judged.** Each category carries a fixed
  valence so the number is reproducible across judges.

## Activation

`HU_EMOTION_REGISTER` (plist env), parsed by `hu_gate_mode_from_env`:

- unset / `off` — nothing rendered, no card read (default)
- `shadow` — the rule is rendered and logged once per process, not sent
- `live` — rule 14 is appended to the casual absolute rules

Do not flip to `live` without (a) a card with n ≥ 100, (b) three nightly
verdicts showing where the gap actually is, and (c) a blind A/B round with
the rule on. A rule that tells the model to be "low-key" when the
measurement says the twin is already too flat would make things worse.

## Running it

```bash
python3 scripts/measure_emotion_card.py --days 60 --max-n 300   # ~10 min on :8741
python3 scripts/eval_emotion_register.py --days 14              # ~1 min
python3 scripts/test_emotion_register.py                        # hermetic
```
