-- Ventura-era (macOS 13) chat.db message-table schema fixture.
-- Adds date_retracted, thread_originator_guid, message_summary_info,
-- balloon_bundle_id, expressive_send_style_id, payload_data,
-- group_action_type, group_title. Does NOT include
-- associated_message_emoji (that's a Sonoma+ addition).

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
