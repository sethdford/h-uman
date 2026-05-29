#!/usr/bin/env python3
"""MessageDisplay hook: cosmetic on-screen secret redaction.

SCOPE (read this before trusting it):
  This is a DISPLAY-ONLY redactor. The Claude Code `MessageDisplay` hook can
  only change what is rendered on YOUR screen via `displayContent`; the
  transcript and what the model sees keep the original text. So this guards
  against shoulder-surfing / screen-shares / screenshots — it does NOT satisfy
  "never log secrets". The real log/transcript guarantee belongs in the
  PreToolUse(Bash) path and the permissions.deny secret globs, not here.

CONTRACT:
  stdin  : the MessageDisplay hook JSON (schema not fully documented; this
           script discovers the message-text field by trying known keys).
  stdout : {"hookSpecificOutput":{"hookEventName":"MessageDisplay",
            "displayContent": "<redacted>"}}  — ONLY when a redaction changed
           the text. Otherwise NOTHING is printed and we exit 0, so the
           original text is shown unchanged. A schema mismatch (none of the
           candidate keys present) is therefore a harmless no-op.

SAFETY:
  - Never raises to the caller: any exception → exit 0, original shown.
  - Conservative patterns only. We deliberately do NOT redact bare 40-char
    hex / base64 blobs because this repo prints git SHAs constantly and that
    would mangle normal output. We target known-shaped credentials only.
  - Set HU_REDACT_DEBUG=1 to append the observed top-level stdin keys to
    /tmp/hu-message-display-keys.log so the real text field can be confirmed
    on a live run (one-time discovery aid; remove once confirmed).
"""
import json
import os
import re
import sys

# Known keys that might carry the assistant message text, most-specific first.
CANDIDATE_KEYS = (
    "displayContent", "message", "content", "text",
    "assistant_message", "messageText", "message_text", "body",
)

# Conservative, known-shaped credential patterns. Each match → "[REDACTED]".
PATTERNS = [
    re.compile(r"sk-ant-[A-Za-z0-9][A-Za-z0-9_-]{12,}"),          # Anthropic API key
    re.compile(r"AIza[0-9A-Za-z_-]{20,}"),                         # Google / Gemini API key
    re.compile(r"AKIA[0-9A-Z]{16}"),                               # AWS access key id
    re.compile(r"xox[baprs]-[A-Za-z0-9-]{10,}"),                   # Slack tokens
    re.compile(r"gh[pousr]_[A-Za-z0-9]{20,}"),                     # GitHub tokens
    re.compile(r"(?i)bearer\s+[A-Za-z0-9._~+/=-]{12,}"),           # Bearer <token>
    re.compile(r"eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}"),  # JWT
    re.compile(r"-----BEGIN[ A-Z]*PRIVATE KEY-----.*?-----END[ A-Z]*PRIVATE KEY-----", re.DOTALL),
    # Assignment form: API_KEY=..., SECRET: "...", PASSWORD = '...'
    re.compile(r"(?i)\b([A-Z0-9_]*(?:API[_-]?KEY|SECRET|TOKEN|PASSWORD|PASSWD))\b(\s*[=:]\s*)(['\"]?)([^\s'\"]{6,})\3"),
]


def redact(text):
    out = text
    for pat in PATTERNS:
        if pat.groups >= 4:  # assignment form — keep the key name, redact the value
            out = pat.sub(lambda m: f"{m.group(1)}{m.group(2)}{m.group(3)}[REDACTED]{m.group(3)}", out)
        else:
            out = pat.sub("[REDACTED]", out)
    return out


def main():
    try:
        raw = sys.stdin.read()
        if not raw.strip():
            return 0
        data = json.loads(raw)
    except Exception:
        return 0  # unparseable input → show original

    if os.environ.get("HU_REDACT_DEBUG") == "1":
        try:
            with open("/tmp/hu-message-display-keys.log", "a") as fh:
                fh.write(json.dumps(sorted(data.keys()) if isinstance(data, dict) else type(data).__name__) + "\n")
        except Exception:
            pass

    if not isinstance(data, dict):
        return 0

    for key in CANDIDATE_KEYS:
        val = data.get(key)
        if isinstance(val, str) and val:
            red = redact(val)
            if red != val:
                json.dump(
                    {"hookSpecificOutput": {
                        "hookEventName": "MessageDisplay",
                        "displayContent": red}},
                    sys.stdout,
                )
            return 0  # found the text field; done (redacted or not)

    return 0  # no known text field → harmless no-op, original shown


if __name__ == "__main__":
    sys.exit(main())
