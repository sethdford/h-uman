# src/multimodal/ — Image, Audio, Video Processing

Multimodal content handling: MIME detection, base64 encoding, image/audio
markers and per-provider content parts. Supports vision-capable providers.

## Key Files

- `multimodal.c` — multimodal pipeline coordination
- `audio.c`, `video.c`, `music.c`, `youtube.c` — per-medium handling

## Rules

- Use `hu_multimodal_detect_mime` before sending content to a provider
- Provider payloads built via `hu_multimodal_build_{openai,anthropic,gemini}_image`
- Validate inputs; return `HU_ERR_INVALID_ARGUMENT` for bad args
