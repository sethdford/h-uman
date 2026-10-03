"""scripts/context_relevance_floor.py on a synthetic vec0 layout (no real db)."""
import importlib.util
import math
import os
import sqlite3
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    "context_relevance_floor", os.path.join(HERE, "..", "scripts", "context_relevance_floor.py"))
crf = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(crf)


def make_db(path, vectors, dim=3, slots=16):
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE memories_vec_rowids(rowid INTEGER PRIMARY KEY, id TEXT,"
                " chunk_id INTEGER, chunk_offset INTEGER)")
    con.execute("CREATE TABLE memories_vec_vector_chunks00(rowid PRIMARY KEY, vectors BLOB)")
    blob = bytearray(slots * dim * 4)
    for i, v in enumerate(vectors):
        struct.pack_into(f"{dim}f", blob, i * dim * 4, *v)
        con.execute("INSERT INTO memories_vec_rowids(id, chunk_id, chunk_offset) VALUES (?,1,?)",
                    (f"k{i}", i))
    con.execute("INSERT INTO memories_vec_vector_chunks00(rowid, vectors) VALUES (1, ?)",
                (bytes(blob),))
    # The real memories_vec is a vec0 virtual table; the loader only parses
    # the float[<dim>] declaration out of its sqlite_master sql.
    con.execute(f"CREATE VIEW memories_vec AS SELECT 'float[{dim}]' AS embedding")
    con.commit()
    con.close()


def test_quantiles_of_known_pairs(tmp_path):
    db = tmp_path / "memory.db"
    vecs = [(1, 0, 0)] * 6 + [(0, 1, 0)] * 6  # pairs: same class 1.0, cross 0.0
    make_db(str(db), vecs)
    out = crf.pair_quantiles(crf.load_vectors(str(db)))
    assert out["n_vectors"] == 12 and out["n_pairs"] == 66
    # 30 same-class pairs (1.0) and 36 cross pairs (0.0): the median is 0.0
    assert out["quantiles"]["p50"] == 0.0
    assert out["quantiles"]["p95"] == 1.0


def test_load_vectors_reads_chunk_layout(tmp_path):
    db = tmp_path / "memory.db"
    make_db(str(db), [(3, 4, 0), (0, 0, 2)])
    vecs = crf.load_vectors(str(db))
    assert len(vecs) == 2
    assert math.isclose(vecs[0][0], 0.6) and math.isclose(vecs[1][2], 1.0)


def test_refuses_without_enough_vectors(tmp_path, capsys):
    db = tmp_path / "memory.db"
    make_db(str(db), [(1, 0, 0), (0, 1, 0)])
    assert crf.main(["--db", str(db)]) == 2
    assert "refusing" in capsys.readouterr().err


def test_refuses_missing_db(tmp_path):
    assert crf.main(["--db", str(tmp_path / "absent.db")]) == 2
