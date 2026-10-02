---
title: Local-Only Mode
---

# Local-Only Mode

`privacy.local_only` keeps conversation content on this machine. The owner
chose this on 2026-10-01. The director, analytical turns, the sensitive and
outage fallbacks, and images all stopped going to Gemini.

## The switch

| Source | Values | Wins over |
|---|---|---|
| `HU_LOCAL_ONLY` env | `1`/`on`/`live`/`true`/`enforce` = enforce, `audit`/`shadow` = audit, `0`/`off`/`false` = off. Any other value logs one WARN and **fails closed to enforce** | everything |
| `privacy.local_only` in config.json | `true` / `false` | the default |
| default (key absent) | enforce when the primary provider's endpoint is local, else off | — |

The absent-key default is enforce only for a local primary. An install whose
primary is a cloud provider would otherwise refuse every reply, so it keeps
working until its owner opts in. The daemon logs the resolved mode once at
startup: `local_only mode=enforce (from default (primary endpoint locality); primary=mlx_local)`.

Locality is decided by the endpoint. Local means a loopback host
(`localhost`, `*.localhost`, `127.0.0.0/8`, `::1`), a unix socket, an
in-process model path, or an in-process backend with no URL (`embedded`,
`coreml`, `mlx`, `llamacpp`, `llama-cli`, `huml`, `apple*`). Two vetoes:

- A gateway that forwards to the cloud is never local, even on loopback:
  `openrouter`, `litellm`, `portkey`, `helicone`, `together`, `groq`,
  `fireworks`, `requesty`.
- A cloud model name is refused even on a loopback URL: `gemini*`, `gpt-*`,
  `chatgpt*`, `claude*`, `grok*`, `o1`/`o3`/`o4`, `*-cloud`, `*:cloud`.

`providers[].local` (`true`/`false`) overrides the endpoint rule for one
provider.

## What enforce changes

| Route | Before | Enforce |
|---|---|---|
| Director / emotion / double-text classify provider | a Gemini provider, `gemini-3.1-flash-lite` (hard-coded, `daemon_director.c`) | the agent's own provider and model; skipped when the primary is not local |
| Analytical/deep tier, router defaults | `gemini-3.1-pro-preview` (hard-coded router default) | the primary model. Models set in `agent.model_router` still apply |
| S3 sensitivity switch | `s3_local_model` / `fallback_model` | no switch |
| On-device-failure retry | `gemini-3.1-flash-lite` | skipped |
| Local-voice failure fallback (daemon T4) | the tier's cloud model | skipped |
| Response-guard slim retry | gemini, then openai | primary only |
| Image description | cloud vision route or the agent provider | no image bytes sent. A bare `[Photo]` becomes `[They sent a photo]` |
| Memory embedder | Vertex `text-embedding` when ADC is set | the local embedder |

## The backstop

`src/core/http.c` checks every request through `hu_http_post_json*`,
`hu_http_post_json_stream` and `hu_http_request`. A model request to a
non-local host is refused with `HU_ERR_PERMISSION_DENIED`. Model requests
are generation, embedding and transcription endpoint shapes. Each refusal
logs one line, with no content:

```
[local_only] refused provider=<host> model=<model> caller=<tag>
```

Feeds, channel APIs and OAuth do not match the model shapes and are never
touched. A refused request is terminal in the reliable provider
(`HU_ERR_PERMISSION_DENIED`), so it is never retried.

The same check runs outside `http.c`:

- **Websockets** (`hu_ws_connect_with_headers`): any non-local websocket is
  refused unless it is an allowed voice service. That covers Gemini Live,
  OpenAI Realtime and the OpenAI `ws_streaming` chat path.
- **Spawned-curl voice paths** (`voice.c`, `cartesia.c`): checked before the
  process is spawned.
- **`web_search` tool:** the query is written from the conversation, so it is
  content and is refused unless `tool:web_search` is on the allow-list.
  `web_fetch` (a URL the model chose) is not gated.

## Voice allow-list

Owner ruling 2026-10-02, "allow voice services only". Under local_only the
only content that may leave is:

- reply text sent to the allowed TTS service for an outgoing voice note;
- inbound audio sent to the allowed STT service.

Neither ever receives memory, persona, history or the system prompt.

```json
"privacy": { "local_only_allow": ["tts:cartesia", "stt:cartesia"] }
```

Services are named `tts:<vendor>` / `stt:<vendor>`. The vendor comes from the
endpoint's host (`api.cartesia.ai` gives `cartesia`, `api.groq.com` gives
`groq`). When the key is absent the default is
`["tts:cartesia", "stt:<voice.stt_provider, else cartesia>"]`. Prod has no
`voice` block, so its default is `stt:cartesia`. Inbound memos in prod are
transcribed on-device by iOS before the daemon sees them, so cloud STT is a
backup path. `[]` allows nothing.

Refused even when listed, because they are model requests:

- Gemini Live and OpenAI Realtime;
- Gemini transcription (`:generateContent`);
- vision and media generation.

The OpenAI `/audio/speech` fallback TTS is refused by default, which leaves
Cartesia as the one TTS. If Cartesia fails, the voice note falls back to text.

`voice.privacy_mode` is unchanged and stricter: it blocks all voice egress,
including Cartesia.

## Caller tags

Refusal and audit lines carry `caller=<tag>`. The tags are `director`,
`agent_turn`, `proactive`, `tools`, `voice`, `inbound_media` and `embedder`.
Anything else shows `unknown`.

## Out of scope

`daemon_cron` shell jobs run whatever command the owner configured. A command
that calls a cloud API is the owner's explicit choice and is not inspected.

## Audit mode

`HU_LOCAL_ONLY=audit` routes exactly as off. The backstop logs
`[local_only audit] would refuse ...` for each request it would block, and
refuses nothing.

**Measurement before enforcing on another install:** run it in audit for 24h
and count the lines:

```bash
grep -c '\[local_only audit\] would refuse' ~/.human/logs/service-loop*.log
grep '\[local_only audit\]' ~/.human/logs/service-loop*.log | sed 's/.*caller=//' | sort | uniq -c
```

Enforce is licensed when every listed caller is one you accept losing.

**Rollback:** set `HU_LOCAL_ONLY=0` in the launchd plist environment and
restart the service, or set `"privacy": {"local_only": false}`. A config
reload (SIGHUP) re-resolves the mode and the allow-list from the file.
