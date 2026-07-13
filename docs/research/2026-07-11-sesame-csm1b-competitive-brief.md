# Competitive Brief: Sesame AI (Maya / CSM-1b) vs h-uman

Date: 2026-07-11. Shelf life: ~1 quarter — Sesame is shipping fast post-Series B.

## TLDR

CSM-1b is not a competitor; it is a **speech synthesis layer** (Apache 2.0 — a
component we could adopt if we ever add a voice channel). Sesame's product, Maya,
excels at **delivery** (voice presence, prosody, timing) and is weak at exactly
h-uman's two moats: **memory** and **being a specific person**. Users publicly
complain Maya forgets them and misfires emotionally (laughed during a serious
personal conversation → churn). Strategy: do not chase their delivery polish on
their turf; finish our measurement loop and own the category they structurally
cannot enter — *indistinguishable from you, in your channels, on your device*.

## Facts (verified 2026-07-11)

- Sesame: Brendan Iribe (Oculus) + Ankit Kumar; $250M Series B (Sequoia, Spark)
  Oct 2025; voice-first iOS app shipped May 2026; smart glasses targeted 2027.
- Maya's stack = **Google Gemma 4 LLM + CSM-1B** (their conversational speech
  model: two Llama-arch autoregressive transformers, text+audio → RVQ audio
  codes, conditioned on conversation history). CSM-1B open-sourced Mar 2025,
  Apache 2.0; clones a voice from ~1 min of audio.
- Their blind tests: indistinguishable from humans on SHORT exchanges; longer
  dialogues degrade (pauses, artifacts, context limits).
- Documented user complaints: no/weak long-term memory ("crucial shortfall"),
  inappropriate affect (laughing at grief), English-mostly, cloud-only.
- Their app copy now promises memory that "grows over time" — they know memory
  is the gap and are racing to close it.

## The convergence

Both products independently chose Gemma as the brain. The LLM is NOT the
battleground for either side. Differentiation lives in the layer around it:
theirs = speech delivery; ours = persona fidelity + relationship memory.
CSM-1b's core idea (condition delivery on conversation history) is the same
insight as our salience arbitration + persona overlays — same principle,
different modality.

## Position they cannot claim

*For a person who wants their digital presence to be genuinely theirs, h-uman
is a personal agent indistinguishable from you — in your actual conversations,
with your actual memory of your actual relationships, on your own hardware.*

Maya must be a branded character at consumer scale, cloud-served, hardware
attached. Per-user persona fidelity in the user's own channels is orthogonal
to their roadmap. Their weakness list is our feature list.

## Strategic implications → backlog mapping

Differentiate (double down):
1. **Measurement loop** — blind-A/B human tier is our credibility engine, as
   their blind tests are theirs. Status: rating-drip send+harvest LIVE
   (2026-07-11), sheet 0/12, first enforcing nightly verdict expected tonight.
2. **Memory moat** — fix starved capture on casual text (dict extractor got
   ~2 beliefs/192 msgs; hu_fact_extract_llm exists unwired — measure yield,
   wire if justified). Contextual proactive outreach is live; no Maya user has
   ever experienced it.
3. **Local-first privacy** — one-sentence differentiator a cloud consumer
   company cannot cheaply copy.

Parity (text analog of "voice presence"):
4. Delivery texture (typing cadence, bubble choreography, fillers) — built;
   calibrate against blind A/B once human tier fills.
5. Latency — shrink the 21KB persona prompt (reorder measured as NO-OP; shrink
   is the lever) + Q6 requant.

Do NOT:
- Compete on generic companion quality / consumer distribution / hardware.
- Rebuild speech synthesis. If voice is ever needed, evaluate CSM-1b locally,
  fine-tuned on own voice (Apache 2.0).
- Re-run steering-vector warmth experiments (closed by measurement 2026-07-09;
  persona overlays are the working register control).

Monitor: Sesame shipping real long-term memory; user-voice cloning / personal
personas; 2027 glasses (ambient context = a memory data source iMessage can't
match).

## Sources

- sesame.com/blog/crossing-the-uncanny-valley-of-voice
- github.com/SesameAILabs/csm
- techcrunch.com/2025/10/21/sesame-the-conversational-ai-startup-from-oculus-founders-raises-250m-and-launches-beta/
- justthink.ai/blog/unveiling-mayas-brain-sesames-new-ai-model (Gemma 4 + CSM-1B stack)
- the-decoder.com/sesame-releases-csm-1b-ai-voice-generator-as-open-source/
- pcworld.com/article/3151873 (review), ailistingtool.com review, App Store
  reviews (memory + affect complaints)
- theaiinsider.tech 2025-11-10 (glasses roadmap), research.contrary.com/company/sesame-ai
