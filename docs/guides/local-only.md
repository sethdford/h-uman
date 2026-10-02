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
| `HU_LOCAL_ONLY` env | `1`/`on`/`live`/`true` = enforce, `audit`/`shadow` = audit, `0`/`off`/`false` = off | everything |
| `privacy.local_only` in config.json | `true` / `false` | the default |
| default (key absent) | enforce when the primary provider's endpoint is local, else off | — |

The absent-key default is enforce only for a local primary. An install whose
primary is a cloud provider would otherwise refuse every reply, so it keeps
working until its owner opts in. The daemon logs the resolved mode once at
startup: `local_only mode=enforce (from default (primary endpoint locality); primary=mlx_local)`.

Locality is decided by the endpoint, never by provider name. Local means a
loopback host (`localhost`, `*.localhost`, `127.0.0.0/8`, `::1`), a unix
socket, or an in-process model path.

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
touched. Not covered: websocket voice (Gemini Live, OpenAI Realtime) and
Cartesia TTS. Those are governed by `voice.privacy_mode` (`core/privacy.h`).

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
restart the service, or set `"privacy": {"local_only": false}`.
