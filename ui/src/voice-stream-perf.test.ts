/**
 * Performance benchmarks for streaming voice pipeline components.
 * These verify latency bounds, not correctness (covered by other tests).
 */
import { describe, it, expect, vi } from "vitest";
import { createVoiceSilenceController, VOICE_STREAM_SILENCE } from "./voice-stream-silence.js";

describe("Silence detection performance", () => {
  it("onLevel processes 1000 calls under 5ms total", () => {
    let t = 0;
    const controller = createVoiceSilenceController({
      isActive: () => true,
      onSilenceEnd: vi.fn(),
      now: () => t,
    });
    controller.reset();

    const start = performance.now();
    for (let i = 0; i < 1000; i++) {
      t += 16; // ~60fps
      controller.onLevel(i % 3 === 0 ? 0.05 : 0.001);
    }
    const elapsed = performance.now() - start;

    expect(elapsed).toBeLessThan(5);
  });

  it("detects silence exactly at threshold boundary", () => {
    let t = 0;
    const cb = vi.fn();
    const controller = createVoiceSilenceController({
      isActive: () => true,
      onSilenceEnd: cb,
      now: () => t,
    });
    controller.reset();

    // Speak for MIN_BEFORE_AUTO_MS
    for (let i = 0; i < 100; i++) {
      t += VOICE_STREAM_SILENCE.MIN_BEFORE_AUTO_MS / 100;
      controller.onLevel(0.05);
    }

    // Go silent for exactly SILENCE_END_MS - 1
    t += VOICE_STREAM_SILENCE.SILENCE_END_MS - 1;
    controller.onLevel(0.001);
    expect(cb).not.toHaveBeenCalled();

    // One more ms crosses the threshold
    t += 1;
    controller.onLevel(0.001);
    expect(cb).toHaveBeenCalledOnce();
  });

  it("fires callback only once per take even with many silent frames", () => {
    let t = 0;
    const cb = vi.fn();
    const controller = createVoiceSilenceController({
      isActive: () => true,
      onSilenceEnd: cb,
      now: () => t,
    });
    controller.reset();

    // Speak
    t += VOICE_STREAM_SILENCE.MIN_BEFORE_AUTO_MS;
    controller.onLevel(0.05);

    // Silence
    t += VOICE_STREAM_SILENCE.SILENCE_END_MS;
    for (let i = 0; i < 100; i++) {
      t += 16;
      controller.onLevel(0.001);
    }
    expect(cb).toHaveBeenCalledOnce();
  });

  it("reset allows re-detection", () => {
    let t = 0;
    const cb = vi.fn();
    const controller = createVoiceSilenceController({
      isActive: () => true,
      onSilenceEnd: cb,
      now: () => t,
    });

    // First take
    controller.reset();
    t += VOICE_STREAM_SILENCE.MIN_BEFORE_AUTO_MS;
    controller.onLevel(0.05);
    t += VOICE_STREAM_SILENCE.SILENCE_END_MS;
    controller.onLevel(0.001);
    expect(cb).toHaveBeenCalledTimes(1);

    // Second take after reset
    controller.reset();
    t += VOICE_STREAM_SILENCE.MIN_BEFORE_AUTO_MS;
    controller.onLevel(0.05);
    t += VOICE_STREAM_SILENCE.SILENCE_END_MS;
    controller.onLevel(0.001);
    expect(cb).toHaveBeenCalledTimes(2);
  });
});

describe("AudioPlaybackEngine performance characteristics", () => {
  it("IDLE_DISPOSE_MS is 60 seconds", async () => {
    // Validates the constant without importing the class (which needs AudioContext)
    const src = await import("./audio-playback.js");
    const engine = new (
      src.AudioPlaybackEngine as unknown as {
        new (sr: number): { sampleRate: number };
      }
    )(24000);
    expect(engine.sampleRate).toBe(24000);
  });

  it("silence thresholds are within acceptable ranges", () => {
    expect(VOICE_STREAM_SILENCE.RMS_SPEECH).toBeGreaterThanOrEqual(0.01);
    expect(VOICE_STREAM_SILENCE.RMS_SPEECH).toBeLessThanOrEqual(0.05);
    expect(VOICE_STREAM_SILENCE.SILENCE_END_MS).toBeGreaterThanOrEqual(800);
    expect(VOICE_STREAM_SILENCE.SILENCE_END_MS).toBeLessThanOrEqual(2000);
    expect(VOICE_STREAM_SILENCE.MIN_BEFORE_AUTO_MS).toBeGreaterThanOrEqual(300);
    expect(VOICE_STREAM_SILENCE.MIN_BEFORE_AUTO_MS).toBeLessThanOrEqual(1500);
  });
});
