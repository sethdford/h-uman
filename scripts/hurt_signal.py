#!/usr/bin/env python3
"""Python twin of hu_hurt_signal_detect (src/daemon/daemon_hurt_handoff.c):
does a message say the contact feels hurt or worried about the owner
("u mad at me?", "why are you being short", "are we ok")?

The phrase lists are read from the C source at import time, so the daemon's
hand-off and the offline metric can never disagree about WHAT counts; only
the small normalization + word-boundary matcher is mirrored here (ASCII-only,
like the C isalpha/isalnum/tolower on bytes). If the C source cannot be read,
load() raises and callers must report the metric as unavailable, never 0.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
C_SOURCE = os.path.join(HERE, "..", "src", "daemon", "daemon_hurt_handoff.c")
SCAN_MAX = 2048  # HU_HURT_SCAN_MAX


def _c_string_array(src, name):
    m = re.search(r"static const char \*const " + re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};",
                  src, re.S)
    if not m:
        raise ValueError(f"{name} not found in {C_SOURCE}")
    items = [bytes(s, "utf-8").decode("unicode_escape") for s in re.findall(r'"((?:[^"\\]|\\.)*)"',
                                                                           m.group(1))]
    if not items:
        raise ValueError(f"{name} is empty in {C_SOURCE}")
    return items


def load(path=C_SOURCE):
    """(phrases, leads, descriptors) from the C source."""
    with open(path, encoding="utf-8") as f:
        src = f.read()
    return (_c_string_array(src, "k_hurt_phrases"), _c_string_array(src, "k_hurt_leads"),
            _c_string_array(src, "k_hurt_descriptors"))


def _is_alpha(ch):
    return ch.isascii() and ch.isalpha()


def _is_alnum(ch):
    return ch.isascii() and ch.isalnum()


def normalize(text):
    """hurt_normalize(): typographic apostrophe -> ', ASCII lowercase, collapse
    whitespace runs, collapse a letter repeated 3+ times to one."""
    text = text.replace("’", "'")[:SCAN_MAX]
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            if out and out[-1] != " ":
                out.append(" ")
            i += 1
            continue
        lc = c.lower() if c.isascii() else c
        if _is_alpha(c):
            run = 1
            while i + run < n and text[i + run].isascii() and text[i + run].lower() == lc:
                run += 1
            if run >= 3:
                out.append(lc)
                i += run
                continue
        out.append(lc)
        i += 1
    return "".join(out)


def _has_word(hay, needle):
    start = 0
    while True:
        k = hay.find(needle, start)
        if k < 0:
            return False
        left = k == 0 or not _is_alnum(hay[k - 1])
        end = k + len(needle)
        right = end == len(hay) or not _is_alnum(hay[end])
        if left and right:
            return True
        start = k + 1


class Detector:
    def __init__(self, path=C_SOURCE):
        self.phrases, self.leads, self.descriptors = load(path)

    def __call__(self, text):
        if not text:
            return False
        norm = normalize(text)
        if any(_has_word(norm, p) for p in self.phrases):
            return True
        if not any(_has_word(norm, lead) for lead in self.leads):
            return False
        return any(_has_word(norm, d) for d in self.descriptors)
