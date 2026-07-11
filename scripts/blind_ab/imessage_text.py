"""Shared iMessage text decoder for macOS chat.db.

Decodes attributedBody (NSAttributedString typedstream format) when
message.text is NULL. Modern macOS stores most message bodies in
attributedBody, not text.
"""


def decode_attributed_body(blob):
    """Best-effort extraction of the message string from a streamtyped
    NSAttributedString archive. Works for the common case; returns None when it
    can't confidently extract (caller skips those rows and reports the count).

    This is the improved decoder from the feat/contact-formality branch (commit e80af898).
    """
    if not blob:
        return None
    try:
        data = bytes(blob)
        # Trim trailing attribute metadata that follows the string payload.
        if b"NSAttributedString" in data and b"NSString" not in data:
            data = data.split(b"NSAttributedString", 1)[1]
        elif b"NSString" in data:
            data = data.split(b"NSString", 1)[1]
        else:
            return None
        # Skip class-version bytes after the marker.
        data = data[5:]
        if not data:
            return None
        # Length prefix: 0x81 -> uint16 LE follows; otherwise a single-byte len.
        if data[0] == 0x81:
            if len(data) < 3:
                return None
            length = int.from_bytes(data[1:3], "little")
            data = data[3:]
        else:
            length = data[0]
            data = data[1:]
        if length <= 0 or length > len(data):
            # Length didn't validate — fall back to a conservative UTF-8 run.
            return None
        text = data[:length].decode("utf-8", errors="replace").strip()
        return text or None
    except Exception:
        return None


def msg_text(text, body):
    """Extract text from a message, preferring text over attributedBody."""
    if text and text.strip():
        return text.strip()
    return decode_attributed_body(body)
