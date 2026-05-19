-- Catalina-era (macOS 10.15) chat.db message-table schema fixture.
-- Pre-Ventura: no date_retracted, no thread_originator_guid, no
-- associated_message_emoji. Used by tests/test_imessage_schema.c
-- (the test embeds the same SQL as a string; this file documents it
-- for human readers and future schema-fixture refactors).

CREATE TABLE message (
  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,
  guid TEXT UNIQUE,
  text TEXT,
  handle_id INTEGER,
  date INTEGER DEFAULT 0,
  is_from_me INTEGER DEFAULT 0,
  associated_message_type INTEGER DEFAULT 0,
  associated_message_guid TEXT,
  attributedBody BLOB,
  date_delivered INTEGER DEFAULT 0,
  date_read INTEGER DEFAULT 0
);
