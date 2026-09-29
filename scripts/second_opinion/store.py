"""The lane's own store (spec §3). Never memory.db: the lane only reads
memory.db and chat.db. Every row carries backend + prompt_version + time."""
import json
import os
import sqlite3
import time

DEFAULT_PATH = os.path.expanduser("~/.human/second_opinion.db")

SCHEMA = """
CREATE TABLE IF NOT EXISTS audits (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  insight_id INTEGER NOT NULL,
  source TEXT NOT NULL,
  verdict TEXT NOT NULL,
  reason TEXT,
  unparseable INTEGER NOT NULL DEFAULT 0,
  backend TEXT NOT NULL,
  prompt_version TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL,
  UNIQUE(insight_id, backend)
);
CREATE TABLE IF NOT EXISTS critiques (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  item_id TEXT NOT NULL,
  weak_source TEXT NOT NULL,
  gaps TEXT NOT NULL,
  missing TEXT,
  severity INTEGER,
  unparseable INTEGER NOT NULL DEFAULT 0,
  backend TEXT NOT NULL,
  prompt_version TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL,
  UNIQUE(item_id, backend)
);
CREATE TABLE IF NOT EXISTS reference_replies (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  contact_id TEXT NOT NULL,
  chat_rowid INTEGER NOT NULL,
  context TEXT NOT NULL,
  reply TEXT NOT NULL,
  rated INTEGER,
  backend TEXT NOT NULL,
  prompt_version TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL,
  UNIQUE(chat_rowid, backend)
);
CREATE TABLE IF NOT EXISTS runs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  started_at_ms INTEGER NOT NULL,
  finished_at_ms INTEGER,
  backend TEXT NOT NULL,
  jobs TEXT NOT NULL,
  exit_code INTEGER,
  manifest TEXT
);
"""


def open_store(path=DEFAULT_PATH):
    if path != ":memory:":
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        # Create the file 0600 BEFORE SQLite opens it: create-then-chmod leaves
        # a window where a file holding message text is group/world readable.
        os.close(os.open(path, os.O_CREAT | os.O_WRONLY, 0o600))
        os.chmod(path, 0o600)  # an older file with a looser mode is tightened too
    con = sqlite3.connect(path)
    con.executescript(SCHEMA)
    return con


def now_ms():
    return int(time.time() * 1000)


def private_open(path, mode="w", **kw):
    """Create/truncate `path` and return it opened, guaranteed 0600 the moment it
    exists — even when a pre-existing file at that path had a looser mode (O_CREAT's
    mode argument is only applied when the file is newly created, so a stale 0644
    file must be re-chmod'd explicitly)."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(fd, 0o600)
    return os.fdopen(fd, mode, **kw)


def _insert(con, sql, args):
    cur = con.execute(sql, args)
    con.commit()
    return cur.rowcount


def add_audit(con, insight_id, source, verdict, reason, unparseable, backend, prompt_version,
              at_ms):
    return _insert(con, "INSERT OR IGNORE INTO audits (insight_id, source, verdict, reason,"
                        " unparseable, backend, prompt_version, created_at_ms)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                   (insight_id, source, verdict, reason, int(unparseable), backend,
                    prompt_version, at_ms))


def audited_ids(con, backend):
    return {r[0] for r in con.execute("SELECT insight_id FROM audits WHERE backend = ?",
                                      (backend,))}


def add_critique(con, item_id, weak_source, gaps, missing, severity, unparseable, backend,
                 prompt_version, at_ms):
    return _insert(con, "INSERT OR IGNORE INTO critiques (item_id, weak_source, gaps, missing,"
                        " severity, unparseable, backend, prompt_version, created_at_ms)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                   (item_id, weak_source, json.dumps(list(gaps)), missing, severity,
                    int(unparseable), backend, prompt_version, at_ms))


def critiqued_items(con, backend):
    return {r[0] for r in con.execute("SELECT item_id FROM critiques WHERE backend = ?",
                                      (backend,))}


def add_reference(con, contact_id, chat_rowid, context, reply, backend, prompt_version, at_ms):
    return _insert(con, "INSERT OR IGNORE INTO reference_replies (contact_id, chat_rowid,"
                        " context, reply, backend, prompt_version, created_at_ms)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?)",
                   (contact_id, chat_rowid, context, reply, backend, prompt_version, at_ms))


def referenced_rowids(con, backend):
    return {r[0] for r in con.execute("SELECT chat_rowid FROM reference_replies"
                                      " WHERE backend = ?", (backend,))}


def start_run(con, backend, jobs, at_ms):
    cur = con.execute("INSERT INTO runs (started_at_ms, backend, jobs) VALUES (?, ?, ?)",
                      (at_ms, backend, jobs))
    con.commit()
    return cur.lastrowid


def finish_run(con, run_id, exit_code, manifest_json, at_ms):
    con.execute("UPDATE runs SET finished_at_ms = ?, exit_code = ?, manifest = ? WHERE id = ?",
                (at_ms, exit_code, manifest_json, run_id))
    con.commit()
