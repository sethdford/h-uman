# Emotional register — a measured axis, local, judge-pinned

**Status:** measurement shipped, prompt rule gated OFF (2026-09-06).

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
