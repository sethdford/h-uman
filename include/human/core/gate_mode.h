#ifndef HU_CORE_GATE_MODE_H
#define HU_CORE_GATE_MODE_H

/* Canonical three-state feature-gate parsing (off | shadow | live).
 *
 * Before 2026-07-12 this parse existed as ~7 per-gate copies
 * (HU_PROMPT_TRIM, HU_GRAPH_GROUNDING, HU_WARMTH_TONE_VOCAB,
 * HU_LLM_FACT_EXTRACT, HU_TOM_DIRECTIVE, HU_SELF_UNCERTAINTY,
 * HU_INTENT_DIRECTIVE) with drifting vocabularies — some accepted
 * "live", some only "on", some matched case-insensitively. This helper
 * is the canonical superset: it never flips a previously-valid config
 * value to a different state, it only accepts MORE spellings of the
 * same operator intent.
 *
 * Contract (per feature-gate-requires-measurement.md):
 *   NULL / ""            -> unset_default (each gate names its own)
 *   "off"                -> OFF
 *   "shadow"             -> SHADOW
 *   "on" | "live" | "1"  -> LIVE
 *   anything else        -> OFF (unknown input must fail closed and
 *                           never activate new behavior)
 * Matching is case-insensitive. */

typedef enum hu_gate_mode {
    HU_GATE_OFF = 0,
    HU_GATE_SHADOW,
    HU_GATE_LIVE,
} hu_gate_mode_t;

/* Pure parse of a gate value per the contract above. */
hu_gate_mode_t hu_gate_mode_parse(const char *value, hu_gate_mode_t unset_default);

/* getenv(env_name) -> hu_gate_mode_parse. */
hu_gate_mode_t hu_gate_mode_from_env(const char *env_name, hu_gate_mode_t unset_default);

#endif /* HU_CORE_GATE_MODE_H */
