#ifndef HU_CORE_PRIVACY_H
#define HU_CORE_PRIVACY_H

#include <stdbool.h>

/*
 * Process-global privacy kill-switch (ADR 2026-05-31: voice provider tiering).
 *
 * When enforced, ALL cloud media egress is blocked regardless of per-call
 * config. Enforced at each egress boundary, so every caller is covered by
 * construction:
 *   - voice STT/TTS            (hu_voice_stt_file, hu_voice_tts)
 *   - Gemini transcription     (hu_voice_stt_gemini)
 *   - cloud vision             (hu_vision_describe_image) + video frame fallback
 *   - cloud TTS synthesis      (hu_cartesia_tts_synthesize — daemon persona path)
 *   - realtime voice providers (Gemini Live / OpenAI Realtime; under privacy
 *                               only the on-device mlx_local backend is allowed)
 *
 * Scope: this covers voice and multimodal MEDIA only. It does NOT make the LLM
 * local — a cloud reasoning provider may still receive conversation TEXT. See
 * the ADR for the exact coverage boundary.
 *
 * It lives in core (not voice) so voice, vision, multimodal, and tts can all
 * consult it without a cross-module dependency.
 *
 * Latched from voice.privacy_mode at config load (hu_voice_config_from_settings,
 * which every entry point — daemon, gateway, CLI — calls) and on reload
 * (hu_agent_reload_config). It is therefore the single source of truth: callers
 * that build partial configs (multimodal audio/video) cannot bypass it, and a
 * stale cached per-call config cannot keep egress blocked after privacy is
 * toggled off — which is why egress decisions read this global, not the per-call
 * config's privacy_mode field (that field is only the latch source).
 */
void hu_privacy_set_enforced(bool enforced);
bool hu_privacy_enforced(void);

#endif /* HU_CORE_PRIVACY_H */
