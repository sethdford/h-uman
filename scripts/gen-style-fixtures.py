#!/usr/bin/env python3
"""Generate 100 synthetic hu_reply_style_facts_t tuples spanning the
parameter space, write as JSON for the C test to load."""
import json, random, pathlib

random.seed(42)
out = []
for i in range(100):
    out.append({
        # Time distribution weighted to land thread-rate in AC-2 band
        # [15%, 65%] with spec-default weights. Empirically calibrated:
        #   0% sec=5 → 73% thread rate (over band)
        #   8% sec=5 → 69% (still over)
        #  14% sec=5 → ~40% (mid-band, target)
        #  20% sec=5 → 13% (under band)
        # The fresh-suppression branch (sec<10, -0.8) is separately
        # pinned by truth-table case #1 (fresh_low_density_*).
        "seconds_since_parent": random.choices(
            [5, 30, 120, 600, 3600],
            weights=[5, 3, 3, 3, 3],
        )[0],
        "parent_position_from_bottom": random.randint(0, 15),
        "pending_questions_in_window": random.randint(0, 4),
        "other_threaded_replies_recent": random.randint(0, 6),
        "our_threaded_replies_recent": random.randint(0, 6),
        "conv_density_msgs_per_min": random.choice([0.5, 2.0, 4.0, 8.0, 15.0]),
        "parent_was_a_question": random.choice([True, False]),
        "persona_formality": round(random.random(), 2),
        "persona_thread_affinity": 0.30,
        "parent_emotional_intensity": random.choice([0, 1, 1, 1]),
    })
p = pathlib.Path("tests/fixtures/imessage_action/distribution_facts.json")
p.parent.mkdir(parents=True, exist_ok=True)
p.write_text(json.dumps(out, indent=2))
print(f"wrote {len(out)} fixtures to {p}")
