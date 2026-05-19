-- Sonoma+ (macOS 14, iOS 17) chat.db message-table schema fixture.
-- Adds associated_message_emoji on top of the Ventura schema. This
-- is the column that gates emoji-typed tapbacks vs the older
-- numeric-only tapback type system.

CREATE TABLE message (
  ROWID INTEGER PRIMARY KEY AUTOINCREMENT,
  guid TEXT UNIQUE,
  text TEXT,
  handle_id INTEGER,
  date INTEGER DEFAULT 0,
  is_from_me INTEGER DEFAULT 0,
  associated_message_type INTEGER DEFAULT 0,
  associated_message_guid TEXT,
  associated_message_emoji TEXT,
  attributedBody BLOB,
  date_delivered INTEGER DEFAULT 0,
  date_read INTEGER DEFAULT 0,
  date_edited INTEGER DEFAULT 0,
  date_retracted INTEGER DEFAULT 0,
  thread_originator_guid TEXT,
  thread_originator_part TEXT,
  message_summary_info BLOB,
  balloon_bundle_id TEXT,
  expressive_send_style_id TEXT,
  payload_data BLOB,
  group_action_type INTEGER DEFAULT 0,
  group_title TEXT
);
