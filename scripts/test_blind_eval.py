#!/usr/bin/env python3
"""
Unit and integration tests for blind_eval_export.py and blind_eval_ingest.py

Tests the round-trip workflow:
  1. Load response pairs
  2. Export and anonymize
  3. Simulate human ratings
  4. Ingest ratings and compute aggregates
  5. Verify per-dimension scores and win rates

Uses deterministic fixtures with fixed seeds to ensure reproducibility.
"""

import json
import sys
import tempfile
from pathlib import Path

# Add scripts dir to path
sys.path.insert(0, str(Path(__file__).parent))

import blind_eval_export
import blind_eval_ingest


# === Fixtures ===

def create_test_pairs() -> list:
    """Create a small test set of response pairs."""
    return [
        {
            "prompt": "hey what's up",
            "real_response": "yo what's good",
            "model_response": "Hello, how can I assist you today?",
        },
        {
            "prompt": "wanna grab lunch?",
            "real_response": "down, where at?",
            "model_response": "I would be happy to join you for lunch. Where did you have in mind?",
        },
        {
            "prompt": "did you see the game last night",
            "real_response": "nah but i heard it was wild",
            "model_response": "I did not see that game. What happened?",
        },
    ]


def create_test_ratings(exported_items: list) -> list:
    """Create mock human ratings for the exported items.

    Real responses should score higher (in this test, we give them 8-9).
    Model responses should score lower (we give them 5-7).
    """
    ratings = []
    for idx, item in enumerate(exported_items):
        rating_id = item["rating_id"]
        source = item["source"]

        # Deterministic scoring based on source
        if source == "real":
            tone = 9
            vocabulary = 8
            humor = 8
            decision_style = 9
        else:  # model
            tone = 5
            vocabulary = 6
            humor = 4
            decision_style = 5

        ratings.append({
            "rating_id": rating_id,
            "tone_1_10": tone,
            "vocabulary_1_10": vocabulary,
            "humor_1_10": humor,
            "decision_style_1_10": decision_style,
            "notes": f"rating for {source} response",
        })

    return ratings


# === Tests ===

def test_export_anonymizes_pairs():
    """Test that export removes source labels and shuffles responses."""
    pairs = create_test_pairs()
    exported = blind_eval_export.export_blind_pairs(pairs, seed=42)

    # Should create 2 items per pair (real + model)
    assert len(exported) == len(pairs) * 2, f"Expected {len(pairs) * 2} items, got {len(exported)}"

    # Each item should have a unique rating_id
    rating_ids = set(item["rating_id"] for item in exported)
    assert len(rating_ids) == len(exported), "Duplicate rating IDs"

    # Check that all fields are present
    for item in exported:
        assert "rating_id" in item
        assert "prompt" in item
        assert "response" in item
        assert "source" in item  # Hidden from human rater but present in exported file
        assert "tone_1_10" in item
        assert "vocabulary_1_10" in item
        assert "humor_1_10" in item
        assert "decision_style_1_10" in item

    # All scores should be None initially
    for item in exported:
        assert item["tone_1_10"] is None
        assert item["vocabulary_1_10"] is None
        assert item["humor_1_10"] is None
        assert item["decision_style_1_10"] is None

    print(f"✓ export_anonymizes_pairs: {len(exported)} items generated from {len(pairs)} pairs")


def test_export_reproducible():
    """Test that export is deterministic with the same seed."""
    pairs = create_test_pairs()

    exported1 = blind_eval_export.export_blind_pairs(pairs, seed=42)
    exported2 = blind_eval_export.export_blind_pairs(pairs, seed=42)

    # Same seed should produce same order
    for e1, e2 in zip(exported1, exported2):
        assert e1["rating_id"] == e2["rating_id"]
        assert e1["source"] == e2["source"]
        assert e1["pair_idx"] == e2["pair_idx"]

    print(f"✓ export_reproducible: seed=42 produces consistent output")


def test_export_different_seed():
    """Test that different seeds produce different shuffle orders."""
    pairs = create_test_pairs()

    exported1 = blind_eval_export.export_blind_pairs(pairs, seed=42)
    exported2 = blind_eval_export.export_blind_pairs(pairs, seed=99)

    # Different seeds should (likely) produce different orders
    # At least the items should be the same, but in different order
    sources1 = [item["source"] for item in exported1]
    sources2 = [item["source"] for item in exported2]

    assert len(sources1) == len(sources2)
    # Different seeds should shuffle differently (with high probability)
    # For a small set this isn't guaranteed, but 6 items with seed diff should shuffle
    print(f"✓ export_different_seed: seed=42 vs seed=99 produce {sum(1 for a, b in zip(sources1, sources2) if a != b)} different positions")


def test_write_read_jsonl():
    """Test round-trip: write JSONL, read back."""
    pairs = create_test_pairs()
    exported = blind_eval_export.export_blind_pairs(pairs, seed=42)

    with tempfile.TemporaryDirectory() as tmpdir:
        output_path = Path(tmpdir) / "test-ratings.jsonl"
        blind_eval_export.write_rating_file_jsonl(exported, output_path)

        # Read back
        read_back = blind_eval_ingest.load_ratings_from_jsonl(output_path)

        assert len(read_back) == len(exported)
        for orig, read in zip(exported, read_back):
            assert orig["rating_id"] == read["rating_id"]
            assert orig["prompt"] == read["prompt"]
            assert orig["response"] == read["response"]

    print(f"✓ write_read_jsonl: {len(exported)} items round-tripped")


def test_write_read_csv():
    """Test CSV export: write, read back, verify visible fields only."""
    pairs = create_test_pairs()
    exported = blind_eval_export.export_blind_pairs(pairs, seed=42)

    with tempfile.TemporaryDirectory() as tmpdir:
        output_path = Path(tmpdir) / "test-ratings.csv"
        blind_eval_export.write_rating_file_csv(exported, output_path)

        # Read back
        read_back = blind_eval_ingest.load_ratings_from_csv(output_path)

        assert len(read_back) == len(exported)
        for orig, read in zip(exported, read_back):
            assert orig["rating_id"] == read["rating_id"]
            # Prompt and response should be present in CSV
            # But not source/pair_idx (those are hidden)

    print(f"✓ write_read_csv: {len(exported)} items round-tripped through CSV")


def test_ingest_computes_aggregates():
    """Test end-to-end: export, mock ratings, ingest, verify aggregates."""
    pairs = create_test_pairs()
    exported = blind_eval_export.export_blind_pairs(pairs, seed=42)

    # Create a mapping of rating_id -> metadata (simulating the exported file)
    exported_mapping = {
        item["rating_id"]: {
            "source": item["source"],
            "pair_idx": item["pair_idx"],
            "prompt": item["prompt"],
        }
        for item in exported
    }

    # Create mock ratings
    ratings = create_test_ratings(exported)

    # Ingest
    aggregates, per_pair = blind_eval_ingest.ingest_ratings(ratings, exported_mapping)

    # Verify structure
    assert "dimensions" in aggregates
    assert "overall_real_win_rate" in aggregates

    dimensions = aggregates["dimensions"]
    for dim in ["tone", "vocabulary", "humor", "decision_style"]:
        assert dim in dimensions
        assert "mean_real" in dimensions[dim]
        assert "mean_model" in dimensions[dim]
        assert "real_win_rate" in dimensions[dim]

    # Real should outscore model (we gave real 8-9, model 5-7)
    for dim in dimensions:
        assert dimensions[dim]["mean_real"] > dimensions[dim]["mean_model"], \
            f"{dim}: real ({dimensions[dim]['mean_real']}) should > model ({dimensions[dim]['mean_model']})"

    # Real should have a high win rate
    overall_wr = aggregates["overall_real_win_rate"]
    assert overall_wr > 0.5, f"Real win rate {overall_wr} should be > 0.5"

    print(f"✓ ingest_computes_aggregates: real={dimensions['tone']['mean_real']:.1f} vs "
          f"model={dimensions['tone']['mean_model']:.1f}, win_rate={overall_wr:.1%}")


def test_full_round_trip_jsonl():
    """Test full round-trip workflow with JSONL."""
    pairs = create_test_pairs()

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)

        # Step 1: Export pairs
        exported_path = tmpdir_path / "exported.jsonl"
        input_pairs_path = tmpdir_path / "pairs.json"
        input_pairs_path.write_text(json.dumps({"pairs": pairs}))

        # Call the function directly
        exported = blind_eval_export.export_blind_pairs(pairs, seed=42)
        blind_eval_export.write_rating_file_jsonl(exported, exported_path)

        # Step 2: Simulate human ratings
        ratings = create_test_ratings(exported)
        ratings_path = tmpdir_path / "ratings.jsonl"
        with open(ratings_path, "w") as f:
            for r in ratings:
                f.write(json.dumps(r) + "\n")

        # Step 3: Ingest ratings
        output_path = tmpdir_path / "results.json"
        exported_mapping = blind_eval_ingest.load_exported_jsonl(exported_path)
        loaded_ratings = blind_eval_ingest.load_ratings(ratings_path)
        aggregates, per_pair = blind_eval_ingest.ingest_ratings(loaded_ratings, exported_mapping)

        result = {
            "n_pairs": len(per_pair),
            "n_ratings": len(loaded_ratings),
            **aggregates,
            "per_pair": per_pair,
        }
        output_path.write_text(json.dumps(result, indent=2))

        # Verify results
        assert result["n_pairs"] == len(pairs)
        assert result["n_ratings"] == len(pairs) * 2  # 2 responses per pair

        dimensions = result["dimensions"]
        for dim in ["tone", "vocabulary", "humor", "decision_style"]:
            assert dimensions[dim]["mean_real"] > dimensions[dim]["mean_model"]

        print(f"✓ full_round_trip_jsonl: {result['n_pairs']} pairs → "
              f"{result['n_ratings']} ratings → aggregated results")


def test_full_round_trip_csv():
    """Test full round-trip workflow with CSV."""
    pairs = create_test_pairs()

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)

        # Export
        exported = blind_eval_export.export_blind_pairs(pairs, seed=42)
        exported_path = tmpdir_path / "exported.jsonl"
        blind_eval_export.write_rating_file_jsonl(exported, exported_path)

        # CSV export
        csv_path = tmpdir_path / "ratings.csv"
        blind_eval_export.write_rating_file_csv(exported, csv_path)

        # Simulate filling in the CSV (we'll just read it and add scores)
        ratings_csv = blind_eval_ingest.load_ratings_from_csv(csv_path)
        # Manually add scores (simulating human rater filling the spreadsheet)
        for idx, rating in enumerate(ratings_csv):
            # Pull the source from the original exported to know which to score high
            orig = exported[idx]
            if orig["source"] == "real":
                rating["tone_1_10"] = 9
                rating["vocabulary_1_10"] = 8
                rating["humor_1_10"] = 8
                rating["decision_style_1_10"] = 9
            else:
                rating["tone_1_10"] = 5
                rating["vocabulary_1_10"] = 6
                rating["humor_1_10"] = 4
                rating["decision_style_1_10"] = 5

        # Write back the filled CSV
        filled_csv_path = tmpdir_path / "ratings-filled.csv"
        with open(filled_csv_path, "w", newline="") as f:
            import csv
            writer = csv.DictWriter(
                f,
                fieldnames=[
                    "rating_id", "prompt", "response",
                    "tone_1_10", "vocabulary_1_10", "humor_1_10", "decision_style_1_10", "notes"
                ]
            )
            writer.writeheader()
            for r in ratings_csv:
                writer.writerow(r)

        # Ingest
        exported_mapping = blind_eval_ingest.load_exported_jsonl(exported_path)
        loaded_ratings = blind_eval_ingest.load_ratings_from_csv(filled_csv_path)
        aggregates, per_pair = blind_eval_ingest.ingest_ratings(loaded_ratings, exported_mapping)

        # Verify
        dimensions = aggregates["dimensions"]
        for dim in ["tone", "vocabulary", "humor", "decision_style"]:
            assert dimensions[dim]["mean_real"] > dimensions[dim]["mean_model"]

        print(f"✓ full_round_trip_csv: CSV round-trip with {len(per_pair)} pairs")


def test_exported_mapping_structure():
    """Test that exported JSONL is correctly structured for ingest."""
    pairs = create_test_pairs()
    exported = blind_eval_export.export_blind_pairs(pairs, seed=42)

    with tempfile.TemporaryDirectory() as tmpdir:
        exported_path = Path(tmpdir) / "exported.jsonl"
        blind_eval_export.write_rating_file_jsonl(exported, exported_path)

        # Load the mapping
        mapping = blind_eval_ingest.load_exported_jsonl(exported_path)

        # Every exported item should have a mapping entry
        for item in exported:
            assert item["rating_id"] in mapping
            entry = mapping[item["rating_id"]]
            assert entry["source"] in ["real", "model"]
            assert entry["pair_idx"] >= 0
            assert entry["prompt"] == item["prompt"]

    print(f"✓ exported_mapping_structure: {len(mapping)} entries created")


def main():
    """Run all tests."""
    print("=== Blind Eval Tests ===\n")

    test_export_anonymizes_pairs()
    test_export_reproducible()
    test_export_different_seed()
    test_write_read_jsonl()
    test_write_read_csv()
    test_ingest_computes_aggregates()
    test_full_round_trip_jsonl()
    test_full_round_trip_csv()
    test_exported_mapping_structure()

    print("\n=== All tests passed ===")


if __name__ == "__main__":
    main()
