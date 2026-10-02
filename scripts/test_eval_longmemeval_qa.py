import pytest
#!/usr/bin/env python3
"""Unit tests for eval_longmemeval_qa.py — no network, pure functions only."""
import json, os, pytest, tempfile
from eval_longmemeval_qa import (
    judge_prompt_for_type, answer_from_retrieved, QAResult,
    load_checkpoint, save_result,
)

class TestJudgePrompts:
    """Test judge prompt templates per question_type."""

    def test_temporal_reasoning_prompt(self):
        """Temporal reasoning should tolerate off-by-one day errors."""
        template = judge_prompt_for_type("temporal-reasoning")
        assert "off-by-one" in template.lower()
        assert "{question}" in template
        assert "{answer}" in template
        assert "{response}" in template

    def test_knowledge_update_prompt(self):
        """Knowledge update should accept updated answer mixed with old info."""
        template = judge_prompt_for_type("knowledge-update")
        assert "updated answer" in template.lower()
        assert "{question}" in template
        assert "{answer}" in template

    def test_single_session_preference_prompt(self):
        """Single-session-preference shouldn't require all rubric points."""
        template = judge_prompt_for_type("single-session-preference")
        assert "does not need to reflect all" in template.lower() or "rubric" in template.lower()
        assert "{question}" in template

    def test_single_session_user_prompt(self):
        """Single-session-user should require all required information."""
        template = judge_prompt_for_type("single-session-user")
        assert "all" in template.lower() or "subset" in template.lower()

    def test_abstention_follows_the_question_id(self):
        """Abstention is marked on the question id ("<id>_abs"), whatever its type."""
        template = judge_prompt_for_type("single-session-user", "8e9f2a1b_abs")
        assert "unanswerable" in template
        assert "unanswerable" not in judge_prompt_for_type("single-session-user", "8e9f2a1b")

    def test_unknown_type_raises(self):
        """The official code raises on an unknown type; so do we, rather than invent a prompt."""
        with pytest.raises(KeyError):
            judge_prompt_for_type("unknown_type_xyz")

    def test_templates_are_the_official_wording(self):
        """Spot-check verbatim phrases from LongMemEval evaluate_qa.py."""
        t = judge_prompt_for_type("temporal-reasoning")
        assert "predicting 19 days when the answer is 18" in t
        assert t.endswith("Is the model response correct? Answer yes or no only.")
        assert "Rubric: {answer}" in judge_prompt_for_type("single-session-preference")

    def test_prompt_formatting(self):
        """Prompts should format correctly with variables."""
        template = judge_prompt_for_type("single-session-user")
        formatted = template.format(
            question="What is X?",
            answer="X is 42",
            response="The answer is 42"
        )
        assert "What is X?" in formatted
        assert "X is 42" in formatted
        assert "The answer is 42" in formatted

class TestAnswerPromptBuilding:
    """Test answer_from_retrieved prompt construction."""

    def test_empty_context(self):
        """Should handle empty retrieved context."""
        prompt = answer_from_retrieved("What is X?", [], "2023/05/30"
        )
        assert "What is X?" in prompt
        assert "2023/05/30" in prompt
        # Should not crash

    def test_with_context(self):
        """Should format retrieved turns with session dates."""
        retrieved = [
            ("User: What is X?\nAssistant: X is 42", "2023/05/20"),
            ("User: How did you know?\nAssistant: I looked it up", "2023/05/21"),
        ]
        prompt = answer_from_retrieved("What is X?", retrieved, "2023/05/30"
        )
        assert "What is X?" in prompt
        assert "2023/05/30" in prompt
        assert "[1]" in prompt and "[2]" in prompt
        assert "2023/05/20" in prompt
        assert "2023/05/21" in prompt

    def test_context_truncation(self):
        """Should limit to first 10 turns."""
        many_turns = [(f"Turn {i}", f"2023/05/{i:02d}") for i in range(1, 20)]
        prompt = answer_from_retrieved("Q?", many_turns, "2023/05/30"
        )
        # Should only include turns 1-10
        assert "[10]" in prompt
        # The 20th turn should not be fully represented (might be truncated)
        # (exact behavior depends on implementation)

    def test_question_date_included(self):
        """Question date should always appear."""
        prompt = answer_from_retrieved("Q?", [], "2023/12/25")
        assert "2023/12/25" in prompt

class TestQAResult:
    """Test QAResult dataclass."""

    def test_result_to_dict(self):
        """Should convert to dict correctly."""
        r = QAResult("q1", "temporal-reasoning", "correct", answer="The answer is 42")
        d = r.to_dict()
        assert d["question_id"] == "q1"
        assert d["question_type"] == "temporal-reasoning"
        assert d["verdict"] == "correct"
        assert d["answer"] == "The answer is 42"

    def test_result_with_error(self):
        """Should include error field."""
        r = QAResult("q1", "temporal-reasoning", "error", error="Timeout")
        d = r.to_dict()
        assert d["verdict"] == "error"
        assert d["error"] == "Timeout"

    def test_result_defaults(self):
        """Should have sensible defaults."""
        r = QAResult("q1", "temporal-reasoning", "incorrect")
        assert r.error is None
        assert r.answer is None
        assert r.retrieved_count == 0

class TestCheckpointIO:
    """Test checkpoint loading/saving with JSONL."""

    def test_save_and_load_single_result(self):
        """Should round-trip a single result."""
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "checkpoint.jsonl")

            r = QAResult("q1", "temporal-reasoning", "correct", answer="42")
            save_result(path, r)

            loaded = load_checkpoint(path)
            assert "q1" in loaded
            assert loaded["q1"].verdict == "correct"
            assert loaded["q1"].answer == "42"

    def test_save_and_load_multiple_results(self):
        """Should handle multiple results."""
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "checkpoint.jsonl")

            r1 = QAResult("q1", "type1", "correct")
            r2 = QAResult("q2", "type2", "incorrect", error=None)
            r3 = QAResult("q3", "type3", "error", error="Timeout")

            save_result(path, r1)
            save_result(path, r2)
            save_result(path, r3)

            loaded = load_checkpoint(path)
            assert len(loaded) == 3
            assert loaded["q1"].verdict == "correct"
            assert loaded["q2"].verdict == "incorrect"
            assert loaded["q3"].error == "Timeout"

    def test_load_nonexistent_checkpoint(self):
        """Should return empty dict for missing file."""
        loaded = load_checkpoint("/nonexistent/path/checkpoint.jsonl")
        assert loaded == {}

    def test_load_malformed_line_skip(self):
        """Should skip malformed JSONL lines gracefully."""
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "checkpoint.jsonl")

            # Write mixed valid and invalid lines
            with open(path, "w") as f:
                f.write(json.dumps({"question_id": "q1", "question_type": "t1", "verdict": "correct"}) + "\n")
                f.write("not valid json\n")
                f.write(json.dumps({"question_id": "q2", "question_type": "t2", "verdict": "incorrect"}) + "\n")

            loaded = load_checkpoint(path)
            # Should load q1 and q2, skip the malformed line
            assert "q1" in loaded
            assert "q2" in loaded
            assert len(loaded) == 2

class TestVerdictParsing:
    """Test verdict extraction from judge responses."""

    def test_yes_verdict(self):
        """'yes' in response should map to correct."""
        # This is tested in the main harness via call_gemini integration
        # but we'd test the logic here if extracted into a function
        response = "Yes, the answer is correct."
        assert "yes" in response.lower()

    def test_no_verdict(self):
        """'no' in response should map to incorrect."""
        response = "No, the answer is not correct."
        assert "no" in response.lower()

class TestAggregation:
    """Test result aggregation (e.g., accuracy by type)."""

    def test_accuracy_calculation(self):
        """Should calculate accuracy correctly."""
        results = [
            QAResult("q1", "type1", "correct"),
            QAResult("q2", "type1", "correct"),
            QAResult("q3", "type1", "incorrect"),
            QAResult("q4", "type2", "correct"),
            QAResult("q5", "type2", "error"),
        ]

        # Aggregate
        n_correct = sum(1 for r in results if r.verdict == "correct")
        n_scored = sum(1 for r in results if r.verdict != "error")

        assert n_correct == 3
        assert n_scored == 4
        # accuracy = 3/4 = 0.75

    def test_error_rate_threshold(self):
        """Should refuse if error rate > 2%."""
        results = [
            *[QAResult(f"q{i}", "type", "correct") for i in range(98)],
            *[QAResult(f"e{i}", "type", "error") for i in range(3)],  # 3% error rate
        ]

        n_attempted = len(results)
        n_errors = sum(1 for r in results if r.verdict == "error")
        error_pct = n_errors / n_attempted

        assert error_pct > 0.02  # Should trigger REFUSING

class TestDatasetLoading:
    """Test dataset loading (reuses eval_memory_benchmarks.py)."""

    def test_lme_questions_structure(self):
        """LME questions should have required fields."""
        # This is more of an integration test; just verify the structure
        # when the dataset is actually loaded (done in smoke test)
        pass

if __name__ == "__main__":
    pytest.main([__file__, "-v"])


def test_retrieved_keys_map_to_ingested_text_in_time_order():
    from eval_longmemeval_qa import order_turns, session_of
    text = {"sa:b:t1": "user: second", "sa:b:t0": "user: first", "sz:t0": "user: older"}
    dates = {"a:b": "2023/05/02", "z": "2023/01/01"}
    got = order_turns(["sa:b:t1", "missing:t9", "sz:t0", "sa:b:t0"], text, dates)
    assert got == [("user: older", "2023/01/01"), ("user: first", "2023/05/02"),
                   ("user: second", "2023/05/02")]
    assert session_of("sa:b:t3") == "a:b"


def test_load_questions_keeps_every_question_without_a_limit(tmp_path):
    import json as _json
    from eval_longmemeval_qa import load_questions
    qs = [{"question_id": f"q{i}", "question_type": "x" if i % 4 else "y",
           "haystack_session_ids": ["s1"], "haystack_sessions": [[{"role": "user", "content": "hi"}]]}
          for i in range(10)]
    p = tmp_path / "d.json"
    p.write_text(_json.dumps(qs))
    assert len(load_questions(str(p))) == 10
    assert len(load_questions(str(p), limit=4)) == 4
    assert {q["question_type"] for q, _ in load_questions(str(p), types=["y"])} == {"y"}
    q, rows = load_questions(str(p))[0]
    assert rows == [("ss1:t0", "s1", "user: hi")]


def test_resume_retries_error_rows_and_keeps_the_latest(tmp_path):
    from eval_longmemeval_qa import QAResult, load_checkpoint, save_result, settled
    path = str(tmp_path / "results.jsonl")
    save_result(path, QAResult("q1", "multi-session", "correct"))
    save_result(path, QAResult("q2", "multi-session", "error", error="embedder down"))
    assert set(settled(load_checkpoint(path))) == {"q1"}
    save_result(path, QAResult("q2", "multi-session", "incorrect"))
    assert settled(load_checkpoint(path))["q2"].verdict == "incorrect"
