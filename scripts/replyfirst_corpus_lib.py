#!/usr/bin/env python3
"""Pure reorder logic for the reply-first corpus. No I/O, no model — unit-tested.

split_deliberation_reply: given a v4-repair raw generation, return (deliberation, reply).
reorder_to_replyfirst:     produce the reply-first training target.
build_target:              full pipeline for one generation; None on parse failure.
format_sft_example:        wrap (prompt, target) into the {"text": ...} SFT schema.
"""
from __future__ import annotations

DEFAULT_SENTINEL = "<|channel|>thought"


def split_deliberation_reply(raw: str, marker: str = "<|channel|>") -> tuple[str, str]:
    """Split a raw generation into (deliberation, reply).

    1. If a channel marker is present, the reply is everything AFTER the last marker
       line; deliberation is everything before it.
    2. Else fall back to paragraph split: last non-empty paragraph = reply.
    3. Single paragraph → pure reply, empty deliberation.
    """
    raw = raw.strip()
    if marker in raw:
        # take text after the final marker occurrence; drop a trailing channel word
        tail = raw.rsplit(marker, 1)[1]
        # strip a leading channel value like "final\n" / "thought\n"
        for word in ("final", "thought", "analysis", "commentary"):
            if tail.lstrip().startswith(word):
                tail = tail.lstrip()[len(word):]
                break
        reply = tail.strip()
        delib = raw.rsplit(marker, 1)[0].strip()
        return delib, reply
    paras = [p.strip() for p in raw.split("\n\n") if p.strip()]
    if len(paras) <= 1:
        return "", raw
    return "\n\n".join(paras[:-1]), paras[-1]


def reorder_to_replyfirst(deliberation: str, reply: str,
                          sentinel: str = DEFAULT_SENTINEL) -> str:
    """Produce target: <reply><sentinel>\\n<deliberation>. Boundary always present."""
    deliberation = deliberation.strip()
    return f"{reply}{sentinel}\n{deliberation}" if deliberation else f"{reply}{sentinel}\n"


def build_target(raw: str, marker: str = "<|channel|>",
                 sentinel: str = DEFAULT_SENTINEL) -> str | None:
    """Full per-generation pipeline. Returns None on parse failure (empty reply)."""
    delib, reply = split_deliberation_reply(raw, marker=marker)
    if not reply.strip():
        return None
    return reorder_to_replyfirst(delib, reply, sentinel=sentinel)


def format_sft_example(prompt: str, target: str) -> dict:
    """Wrap into the {"text": ...} schema training_loop.py expects (see :611-634)."""
    return {"text": f"{prompt}\n{target}"}
