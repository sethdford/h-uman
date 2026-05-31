#ifndef HU_CORE_PRIVACY_H
#define HU_CORE_PRIVACY_H

#include <stdbool.h>

/*
 * Process-global privacy kill-switch (ADR 2026-05-31: voice provider tiering).
 *
 * When enforced, ALL cloud media egress must be blocked regardless of per-call
 * config — voice STT/TTS, multimodal Gemini transcription/description, and cloud
 * vision (hu_vision_describe_image). It lives in core (not voice) so voice,
 * vision, and multimodal can all consult it without a cross-module dependency.
 *
 * Latched once at config load (hu_voice_config_from_settings, which every entry
 * point — daemon, gateway, CLI — calls) from voice.privacy_mode, so callers that
 * build partial configs (multimodal audio/video) cannot bypass it.
 */
void hu_privacy_set_enforced(bool enforced);
bool hu_privacy_enforced(void);

#endif /* HU_CORE_PRIVACY_H */
