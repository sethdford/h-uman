#!/usr/bin/env python3
"""
Deterministic humanness scorer runner for CI.

Runs the deterministic humanness axes from `human eval score` over a committed
synthetic prompt set and emits a machine-readable scores file.

Only axes with real evaluated data (n > 0) on the synthetic set are emitted, so
the gate never contains a vacuous always-0.0 metric:
  - anti_ai: deterministic structural AI-tell classifier (the shape scorer).
             Always available on text → the load-bearing deterministic signal.
  - relationship / fidelity: emitted ONLY if the eval reports n > 0 (they need a
             target register/persona absent from this synthetic set, so today
             they are excluded here and belong to the scheduled secrets/persona
             tier — see specs/measurement-loop/tasks.md task 5-6).

This script is invoked by the .github/workflows/humanness.yml PR gate.
It requires the build/human binary to exist.
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def get_project_root():
    """Return the absolute path to the project root."""
    script_dir = Path(__file__).resolve().parent
    return script_dir.parent


def get_synthetic_prompts():
    """
    Return the committed synthetic prompt set for deterministic evaluation.

    These prompts represent a fixed set of test cases that should produce
    identical scores across runs (determinism requirement for AC-1/D6).
    """
    return [
        {
            "reply": "hey im good, just busy with work stuff. you?",
            "channel": "imessage",
        },
        {
            "reply": "That's a great question. Let me break this down for you:\n\n- First point\n- Second point\n- Third point\n\nHope this helps!",
            "channel": "imessage",
        },
        {
            "reply": "Certainly! I would be delighted to assist you with that. As an AI, I understand the importance of clear communication.",
            "channel": "imessage",
        },
        {
            "reply": "quick thought: the original proposal makes more sense if we consider the long-term implications. pushing back hard on this.",
            "channel": "slack",
        },
        {
            "reply": "**Summary:**\n\nHere's what happened:\n1. Event A occurred\n2. Event B followed\n3. Conclusion C\n\nFeel free to reach out if you have questions.",
            "channel": "email",
        },
        {
            "reply": "lol that's hilarious 😂 no way that's happening",
            "channel": "imessage",
        },
        {
            "reply": "I'd be happy to help with that. Please let me know if you need anything else.",
            "channel": "imessage",
        },
        {
            "reply": "been thinking about what you said. youre onto something real there.",
            "channel": "imessage",
        },
    ]


def run_eval_score(project_root, jsonl_input):
    """
    Run `human eval score` over the provided JSONL input.

    Returns a dict with axes containing {mean, stderr, n, available}.
    """
    human_binary = project_root / "build" / "human"
    if not human_binary.exists():
        raise FileNotFoundError(f"human binary not found at {human_binary}")

    # Create a temporary file for the JSONL input
    with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl", delete=False) as f:
        for obj in jsonl_input:
            f.write(json.dumps(obj) + "\n")
        jsonl_path = f.name

    try:
        result = subprocess.run(
            [str(human_binary), "eval", "score", "--in", jsonl_path],
            capture_output=True,
            text=True,
            timeout=30,
        )

        if result.returncode != 0:
            print(f"[humanness runner] eval score failed: {result.stderr}", file=sys.stderr)
            raise RuntimeError(f"human eval score exited with code {result.returncode}")

        # Parse the JSON output from stdout
        output_lines = result.stdout.strip().split("\n")
        json_line = None
        for line in output_lines:
            if line.startswith("{"):
                json_line = line
                break

        if not json_line:
            raise ValueError("No JSON output from eval score")

        return json.loads(json_line)
    finally:
        Path(jsonl_path).unlink(missing_ok=True)


def compute_aggregate_score(axes):
    """
    Compute an aggregate humanness score from the axes.

    Axes returned by eval score:
      - anti_ai: [0, 1], higher is more human (avoids AI patterns)
      - relationship: [0, 1], higher is contextually attuned
      - fidelity: [0, 1], higher is matches target style (if target supplied)

    For deterministic tier, we weight equally and average.
    """
    scores = []

    # anti_ai: structural classifier, always available
    if "anti_ai" in axes and axes["anti_ai"]["n"] > 0:
        scores.append(axes["anti_ai"]["mean"])

    # relationship: relationship axis scorer, always available
    if "relationship" in axes and axes["relationship"]["n"] > 0:
        scores.append(axes["relationship"]["mean"])

    # fidelity: requires target_style, won't be available in deterministic runner
    # (we use fixed synthetic data, not a specific target persona)
    if "fidelity" in axes and axes["fidelity"]["available"] and axes["fidelity"]["n"] > 0:
        scores.append(axes["fidelity"]["mean"])

    if not scores:
        return 0.0

    return sum(scores) / len(scores)


def main():
    """Main entry point."""
    project_root = get_project_root()

    # Get the fixed synthetic prompt set
    prompts = get_synthetic_prompts()

    # Run the eval score command
    print(f"[humanness runner] evaluating {len(prompts)} synthetic prompts...", file=sys.stderr)
    eval_result = run_eval_score(project_root, prompts)

    # Extract axes
    axes = eval_result.get("axes", {})

    # Compute aggregate score
    aggregate = compute_aggregate_score(axes)

    # Build the output JSON. Include a metric ONLY when it has real evaluated
    # data (axis n > 0, and for fidelity also `available`). A metric with n == 0
    # — e.g. `relationship` on a synthetic set with no target register — is NOT
    # signal and must never appear in a gate as a vacuous 0.0 that can't fail.
    metrics = {}
    if axes.get("anti_ai", {}).get("n", 0) > 0:
        metrics["anti_ai"] = axes["anti_ai"]["mean"]
    if axes.get("relationship", {}).get("n", 0) > 0:
        metrics["relationship"] = axes["relationship"]["mean"]
    if axes.get("fidelity", {}).get("available") and axes.get("fidelity", {}).get("n", 0) > 0:
        metrics["fidelity"] = axes["fidelity"]["mean"]

    output = {
        "metrics": metrics,
        "aggregate": aggregate,
        "n_evaluated": eval_result.get("n", 0),
    }

    # Print JSON output
    print(json.dumps(output, indent=2))

    return 0


if __name__ == "__main__":
    sys.exit(main())
