# Dermot Humanness Recovery — Release Notes

## C1 — Self-RAG ABSTAINED pass-through (shipped, live)

Commit `9a5911f9` on `main`. Score-based ABSTAINED (low-confidence /
unknown-fact) now passes the original draft through instead of substituting
the canned `"I don't have memory backing this. Want to tell me?"` template.
Policy refusal (`<refuse>` → `HU_REFUSAL_POLICY`) is untouched and still
substitutes. Verifier scoring + telemetry counters
(`self_rag_abstentions`) remain; only `self_rag_refusals_rendered` semantics
narrowed to the policy path. Daemon rebuilt + restarted via atomic-mv
install script.

## C2 — Conversational + reflexive tiers → mlx_local Seth-voice LoRA (config-only)

**Decision: implemented as a config change, NOT the T7–T11 probe code.**

### What changed

`~/.human/config.json` `agent.model_router`:

```diff
-      "reflexive_model": "gemini-3.1-flash-lite-preview",
-      "conversational_model": "gemini-3.5-flash",
+      "reflexive_model": "gemma-4-31b-it-4bit",
+      "conversational_model": "gemma-4-31b-it-4bit",
       "analytical_model": "gemini-3.1-pro-preview",
       "deep_model": "gemini-3.1-pro-preview",
```

(operator config — NOT committed to the repo. Backup at
`~/.human/config.json.bak.c2-<ts>`.)

### Why config-only beats the spec's probe design (T7–T11)

The `default_provider` is `reliable` with:
- `primary_provider = mlx_local` (http://127.0.0.1:8741/v1)
- `fallback_providers = [gemini]`
- `model_fallbacks: gemma-4-31b-it-4bit → gemini-3.5-flash`

`reliable_chat` walks `[model, ...model_fallbacks]` and, for each model,
tries primary (mlx_local) then each fallback provider. So routing a tier to
`gemma-4-31b-it-4bit`:

1. mlx_local serves gemma + Seth-LoRA (loaded server-side via mlx-admin
   adapter swap) → **Seth voice** (AC-5, AC-6, AC-12).
2. If mlx_local is down → chain auto-falls to `gemini-3.5-flash` on the
   gemini provider → **cloud fallback** (AC-8).

Analytical/Deep stay on `gemini-3.1-pro-preview` (AC-7).

This reuses existing, tested fallback machinery. No new `hu_mlx_local_probe`,
no new router fields, no new config parsing — satisfies AC-5/6/7/8/12 with
zero code risk (KISS/YAGNI: the reliable provider already owns fallback).

### Corrected a latent bug in the spec

Spec T11 suggested `mlx_local_model = "seth-lora-v4-repair-20260525-071921"`
(the **adapter** name). mlx-server reports its model id as
`gemma-4-31b-it-4bit`; the Seth voice comes from the adapter already loaded
server-side. Sending the adapter name as the wire `model` would make the
server reject it. The correct wire name is `gemma-4-31b-it-4bit`.

### Tradeoff accepted

When mlx-server is down, the reliable provider wastes ~1 s (2 retries ×
500 ms backoff) on the dead primary before falling to cloud. The spec's
probe would avoid that by routing straight to Gemini. Accepted because
mlx-server is a managed launchd service (`ai.human.mlx-server`) that is
effectively always up; the penalty only applies during a restart window.

### Verification

- `curl` to mlx_local `/v1/chat/completions` with `model=gemma-4-31b-it-4bit`
  and Dermot's "Going out with a bang?!" → reply `"more like a quiet
  whimper"` (Seth-voiced, dry, NOT the canned template). finish_reason=stop;
  M3 thinking-headroom strip working (completion_tokens=466 thinking,
  visible reply short + clean).
- Daemon restarted clean (adapter loaded, iMessage poll active).
- Live daemon `model route: gemma-4-31b-it-4bit` line confirmed on next
  inbound conversational/reflexive message.

### To revert

Restore `~/.human/config.json.bak.c2-<ts>` and `launchctl kickstart -k
gui/$UID/ai.human.service-loop`.

## C3 — Native iMessage threaded-reply AX wiring

(in progress — see tasks T12–T16)
