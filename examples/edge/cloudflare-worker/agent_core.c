/**
 * WASM decision core: host computes simple text features, core returns policy.
 * Keeps networking/secrets in edge host while logic stays in WASM.
 *
 * Policies: 0 = concise, 1 = detailed, 2 = urgent.
 */

#include <stdint.h>

__attribute__((export_name("choose_policy")))
uint32_t choose_policy(uint32_t text_len, uint32_t has_question,
                       uint32_t has_urgent_keyword, uint32_t has_code_hint) {
    uint32_t urgent_bonus = (text_len > 900) ? 1 : 0;
    uint32_t detailed_bonus = (text_len > 260) ? 1 : 0;
    uint32_t urgent_score = has_urgent_keyword * 3 + urgent_bonus;
    uint32_t detailed_score = has_question * 2 + has_code_hint * 2 + detailed_bonus;

    if (urgent_score >= 3) return 2;
    if (detailed_score >= 3) return 1;
    return 0;
}
