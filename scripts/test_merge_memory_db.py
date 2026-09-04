import os, sqlite3, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location("mm", os.path.join(HERE, "merge-memory-db.py"))
mm = importlib.util.module_from_spec(spec); spec.loader.exec_module(mm)

def mk(path, rows, extra_table=False):
    c = sqlite3.connect(path)
    c.execute("CREATE TABLE memories(id INTEGER PRIMARY KEY AUTOINCREMENT, key TEXT UNIQUE, content TEXT)")
    c.execute("CREATE TABLE kv(k TEXT PRIMARY KEY, v TEXT)")
    c.executemany("INSERT INTO memories(key, content) VALUES(?,?)", rows)
    c.execute("INSERT INTO kv VALUES('shared','base')")
    if extra_table:
        c.execute("CREATE TABLE only_in_delta(x INTEGER)"); c.execute("INSERT INTO only_in_delta VALUES(7)")
        c.execute("INSERT INTO kv VALUES('new','delta')")
    c.commit(); c.close()

def test_merge_dedupes_by_unique_key_and_creates_delta_only_tables():
    d = tempfile.mkdtemp()
    base, delta, out = [os.path.join(d, n) for n in ("base.db", "delta.db", "out.db")]
    mk(base, [("k1", "one"), ("k2", "two")])
    mk(delta, [("k2", "two-dup-with-id-1"), ("k3", "three")], extra_table=True)
    mm.merge(base, delta, out, must_grow=("memories",))
    c = sqlite3.connect(out)
    keys = [r[0] for r in c.execute("SELECT key FROM memories ORDER BY key")]
    assert keys == ["k1", "k2", "k3"], keys                     # k2 once, not twice
    assert c.execute("SELECT content FROM memories WHERE key='k2'").fetchone()[0] == "two"  # base wins
    assert c.execute("SELECT x FROM only_in_delta").fetchone()[0] == 7
    assert c.execute("SELECT COUNT(*) FROM kv").fetchone()[0] == 2
    assert c.execute("PRAGMA quick_check").fetchone()[0] == "ok"

def test_refuses_when_must_grow_table_did_not_grow():
    d = tempfile.mkdtemp()
    base, delta, out = [os.path.join(d, n) for n in ("base.db", "delta.db", "out.db")]
    mk(base, [("k1", "one")]); mk(delta, [("k1", "one")])
    try:
        mm.merge(base, delta, out, must_grow=("memories",)); assert False, "should refuse"
    except SystemExit as e:
        assert "did not grow" in str(e)
