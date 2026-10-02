#!/usr/bin/env python3
# scripts/chat_template_rows.py
#
# Renders preference rows ({prompt, chosen, rejected}) through the SERVING
# model's chat template, so a persona adapter trains on the exact token stream
# production feeds it.
#
# WHY (2026-10-01): production renders every GLM-4.5-Air prompt with
#   tokenizer.apply_chat_template(messages, add_generation_prompt=True,
#                                 enable_thinking=False)
# (gemma-realtime-1/scripts/mlx-server.py::prepare_prompt_lm), so the model
# sees  [gMASK]<sop>...<|user|>\n...{text}/nothink<|assistant|>\n<think></think>
# and is expected to continue with  \n{reply}  and then end the turn. Neither
# training path matched that:
#   - mlx_tune's ORPO/SimPO/KTO trainers tokenize the raw string
#     `prompt + chosen` (mlx_tune/rl_trainers.py ~905/1641/1510): no template
#     markers, no <think></think>, no stop token.
#   - mlx_lm_lora's ORPODataset (the trainer that actually built the served
#     seth-glm-air-mlxtune-orpo-20260905-* adapter) DOES call
#     apply_chat_template, but without enable_thinking=False (no `/nothink` on
#     the user turn), with no system prompt, and with nothing after the reply
#     -- so the end of the assistant turn is never a training target.
# Neither ever trains "reply, THEN stop". On classifier-style prompts the served
# adapter ends the turn immediately (EOS or </think>) on ~13% of requests, which
# drives cloud failover.
#
# THE FIX lives here, in our data, not in site-packages: each row becomes
#   prompt   = apply_chat_template(messages, add_generation_prompt=True,
#                                  enable_thinking=False)            (as text)
#   chosen   = <template continuation for the reply> + END_OF_TURN    (as text)
#   rejected = <template continuation for the reply> + END_OF_TURN    (as text)
# The continuation is DERIVED from the template (render the conversation with
# the reply appended, strip the generation-prompt prefix), not hand-written, so
# a template change can't silently drift from production.
#
# mlx_tune then tokenizes `prompt + chosen` with tokenizer.encode(), which for
# GLM adds NO special tokens (tokenizer.json post_processor is ByteLevel only)
# and matches the added tokens ([gMASK], <sop>, <|user|>, ...) inside the text
# as single ids. So no BOS is re-added and nothing is mangled -- asserted, not
# assumed, by check_trainer_tokenization() and scripts/test_chat_template_rows.py.
#
# TOKENIZER ONLY: load_serving_tokenizer() uses mlx_lm.utils.load_tokenizer,
# which fetches only *.json/*.jinja/... (never *.safetensors), with
# HF_HUB_OFFLINE=1 so it can only read the local HF cache.
"""Render preference rows through the serving model's chat template."""

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

DEFAULT_MODEL = "mlx-community/GLM-4.5-Air-4bit"

# What every production GLM prompt ends with (mlx-server.py prepare_prompt_lm,
# GEMMA_DISABLE_THINKING=1 -> enable_thinking=False). Verified 2026-10-01 by
# rendering through the local tokenizer; pinned by the test suite.
PRODUCTION_SUFFIX = "<|assistant|>\n<think></think>"
PRODUCTION_PREFIX = "[gMASK]<sop>"

# End-of-turn marker appended to every reply. Evidence (GLM-4.5-Air-4bit
# snapshot 60837794f3ca, read 2026-10-01):
#   - config.json / generation_config.json eos_token_id = [151329, 151336,
#     151338] = <|endoftext|>, <|user|>, <|observation|>; mlx_lm.load() makes
#     all three generation stops, so mlx-server ends a reply on any of them.
#   - chat_template.jinja has no explicit end-of-assistant marker; in every
#     multi-turn render the token that follows an assistant reply is
#     `<|user|>` -- that is the turn boundary the base model was trained on.
# tokenizer_config.json's eos_token is <|endoftext|> (end of DOCUMENT); we use
# the turn delimiter instead. Override with --end-of-turn if that ever changes.
END_OF_TURN = "<|user|>"

# Strings that must never appear INSIDE reply content: if they do, the row was
# harvested with template scaffold in it (e.g. a production reply that leaked
# "</think>", or a pre-templated row being templated twice). The template
# itself splits on '</think>' and would silently move text into reasoning.
SCAFFOLD_MARKERS = (
    "[gMASK]", "<sop>", "<|system|>", "<|user|>", "<|assistant|>",
    "<|observation|>", "<|endoftext|>", "<think>", "</think>", "/nothink",
)

FORMAT_ID = "glm-chat-template-v1"
MANIFEST_NAME = "chat_template_manifest.json"


class RowRejected(ValueError):
    """A row that cannot be rendered faithfully. Dropped and counted, never
    silently trained on. `key` is the stable reason used for counting."""

    def __init__(self, key, detail=None):
        super().__init__(f"{key}: {detail}" if detail else key)
        self.key = key


# --------------------------------------------------------------------------
# Tokenizer
# --------------------------------------------------------------------------


def load_serving_tokenizer(model_id=DEFAULT_MODEL):
    """The mlx_lm TokenizerWrapper production and mlx_tune both use, loaded
    from tokenizer files only (no weights, no network)."""
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    from mlx_lm.utils import load_tokenizer

    return load_tokenizer(model_id)


def tokenizer_fingerprint(tokenizer):
    """sha256 of the chat template the rows were rendered with -- recorded in
    the manifest so a later template change is visible."""
    tmpl = getattr(tokenizer, "chat_template", None) or ""
    return hashlib.sha256(tmpl.encode("utf-8")).hexdigest()[:16]


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------


def _template(tokenizer, messages, add_generation_prompt):
    return tokenizer.apply_chat_template(
        messages,
        tokenize=False,
        add_generation_prompt=add_generation_prompt,
        enable_thinking=False,
    )


def row_messages(row, system=None):
    """The conversation a row represents, in production's message shape.

    `messages` (a list) wins if present; otherwise an optional system turn
    (row['system'], else the `system` argument) followed by one user turn
    holding row['prompt']."""
    if isinstance(row.get("messages"), list) and row["messages"]:
        return [dict(m) for m in row["messages"]]
    if not isinstance(row.get("prompt"), str) or not row["prompt"].strip():
        raise RowRejected("empty or non-string prompt")
    msgs = []
    for marker in SCAFFOLD_MARKERS:
        if marker != "/nothink" and marker in row["prompt"]:
            raise RowRejected(f"prompt contains template scaffold {marker!r}")
    sys_text = row.get("system") if isinstance(row.get("system"), str) else system
    if sys_text:
        msgs.append({"role": "system", "content": sys_text})
    msgs.append({"role": "user", "content": row["prompt"]})
    return msgs


def render_prompt(tokenizer, messages):
    """Exactly what mlx-server.py's prepare_prompt_lm builds for `messages`."""
    if messages and messages[-1].get("role") == "assistant":
        raise RowRejected("conversation already ends with an assistant turn")
    return _template(tokenizer, messages, add_generation_prompt=True)


def render_completion(tokenizer, messages, prompt_text, content, end_of_turn=END_OF_TURN,
                      allow_empty=False):
    """The text the model must emit after `prompt_text` to produce `content`
    and end its turn: the template's own rendering of the reply, plus the
    end-of-turn marker."""
    if not isinstance(content, str):
        raise RowRejected("reply is not a string")
    for marker in SCAFFOLD_MARKERS:
        if marker in content:
            raise RowRejected(f"reply contains template scaffold {marker!r}")
    if not content.strip() and not allow_empty:
        raise RowRejected("empty reply would teach an immediate end-of-turn")
    full = _template(tokenizer, messages + [{"role": "assistant", "content": content}],
                     add_generation_prompt=False)
    if not full.startswith(prompt_text):
        raise RowRejected("template render of the reply does not extend the generation prompt")
    return full[len(prompt_text):] + end_of_turn


def format_pair(tokenizer, row, system=None, end_of_turn=END_OF_TURN):
    """One raw {prompt, chosen, rejected} row -> one templated row.

    An empty `rejected` is kept on purpose: "\\n" absent, just END_OF_TURN --
    i.e. "ending the turn immediately is the WORSE answer", which is precisely
    the failure this format exists to fix. An empty `chosen` is refused."""
    messages = row_messages(row, system=system)
    prompt_text = render_prompt(tokenizer, messages)
    if not prompt_text.startswith(PRODUCTION_PREFIX) or not prompt_text.endswith(PRODUCTION_SUFFIX):
        raise RowRejected("rendered prompt does not match the production prefix/suffix")
    out = {
        "prompt": prompt_text,
        "chosen": render_completion(tokenizer, messages, prompt_text, row.get("chosen"),
                                    end_of_turn=end_of_turn),
        "rejected": render_completion(tokenizer, messages, prompt_text, row.get("rejected"),
                                      end_of_turn=end_of_turn, allow_empty=True),
    }
    if out["chosen"] == out["rejected"]:
        raise RowRejected("chosen == rejected after rendering")
    for k, v in row.items():  # carry metadata (weights, ids) through untouched
        if k not in ("prompt", "chosen", "rejected", "messages", "system"):
            out[k] = v
    return out


# --------------------------------------------------------------------------
# Trainer-side contract
# --------------------------------------------------------------------------


def token_ids(tokenizer, text):
    """Tokenize the way mlx_tune does: plain encode() with default special-token
    handling (rl_trainers.py `self.tokenizer.encode(prompt + chosen)`)."""
    return list(tokenizer.encode(text))


def check_trainer_tokenization(tokenizer, row, max_seq_length, end_of_turn=END_OF_TURN):
    """Assert the trainer will see what production sees. Returns the longer
    side's token count. Raises RowRejected with the reason otherwise.

    - exactly one [gMASK] and it is token 0 (encode() re-added nothing);
    - the prompt's tokens are an exact prefix of prompt+reply (no BPE merge
      across the boundary, so the trainer's prompt/response split is clean);
    - the LAST token is the end-of-turn id -- and the sequence fits
      max_seq_length, because mlx_tune truncates from the RIGHT, which would
      cut exactly that token off."""
    gmask = tokenizer.convert_tokens_to_ids("[gMASK]")
    eot = tokenizer.convert_tokens_to_ids(end_of_turn)
    p_ids = token_ids(tokenizer, row["prompt"])
    longest = 0
    for side in ("chosen", "rejected"):
        ids = token_ids(tokenizer, row["prompt"] + row[side])
        if ids[:1] != [gmask] or ids.count(gmask) != 1:
            raise RowRejected("encode() did not yield exactly one leading [gMASK]", side)
        if ids[: len(p_ids)] != p_ids:
            raise RowRejected("prompt tokens are not a prefix of prompt+reply", side)
        if ids[-1] != eot:
            raise RowRejected("last token is not the end-of-turn id",
                              f"{side}: got {ids[-1]}, want {eot}")
        if len(ids) > max_seq_length:
            raise RowRejected("longer than max_seq_length (right-truncation would drop "
                              "the end-of-turn token)", f"{side}: {len(ids)} > {max_seq_length}")
        longest = max(longest, len(ids))
    return longest


def is_templated_row(row, end_of_turn=END_OF_TURN):
    """Cheap, tokenizer-free check that a row is already in this format."""
    return (isinstance(row.get("prompt"), str)
            and row["prompt"].startswith(PRODUCTION_PREFIX)
            and row["prompt"].endswith(PRODUCTION_SUFFIX)
            and all(isinstance(row.get(k), str) and row[k].endswith(end_of_turn)
                    for k in ("chosen", "rejected")))


def templated_report(jsonl_path):
    """{count, templated, raw} for a corpus file -- used by mlx_tune_train.py
    to refuse training a GLM adapter on raw rows."""
    n = t = 0
    with open(jsonl_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            n += 1
            t += is_templated_row(json.loads(line))
    return {"count": n, "templated": t, "raw": n - t}


# --------------------------------------------------------------------------
# Corpus conversion
# --------------------------------------------------------------------------


def _median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else None


def convert_file(tokenizer, src, dst, max_seq_length, system=None, end_of_turn=END_OF_TURN):
    """Template one JSONL file. Besides kept/dropped counts, reports the token
    shape that matters for training: prompt length, and the chosen reply's
    share of its sequence. mlx-tune's ORPO NLL is divided by the FULL sequence
    length (prompt included), so a long system prompt dilutes the reply/stop
    signal by roughly that share."""
    kept, dropped, reasons, longest = 0, 0, {}, 0
    prompt_toks, reply_share = [], []
    with open(src) as fin, open(dst, "w") as fout:
        for line in fin:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            try:
                out = format_pair(tokenizer, row, system=system, end_of_turn=end_of_turn)
                longest = max(longest, check_trainer_tokenization(
                    tokenizer, out, max_seq_length, end_of_turn=end_of_turn))
            except RowRejected as e:
                dropped += 1
                reasons[e.key] = reasons.get(e.key, 0) + 1
                continue
            p = len(token_ids(tokenizer, out["prompt"]))
            total = len(token_ids(tokenizer, out["prompt"] + out["chosen"]))
            prompt_toks.append(p)
            reply_share.append(round((total - p) / total, 4))
            fout.write(json.dumps(out, ensure_ascii=False) + "\n")
            kept += 1
    return {"src": str(src), "dst": str(dst), "kept": kept, "dropped": dropped,
            "drop_reasons": reasons, "max_tokens": longest,
            "prompt_tokens_median": _median(prompt_toks),
            "prompt_tokens_max": max(prompt_toks) if prompt_toks else None,
            "chosen_reply_share_median": _median(reply_share)}


def kept_fraction_failures(files, min_kept_frac):
    """Messages for every file (train AND valid) that kept too few rows. A
    valid split that silently shrank would make every held-out number
    downstream describe a different set."""
    fails = []
    for name, rep in files.items():
        total = rep["kept"] + rep["dropped"]
        if total == 0 or rep["kept"] == 0:
            fails.append(f"{name}: no usable rows")
        elif rep["kept"] / total < min_kept_frac:
            fails.append(f"{name}: only {rep['kept']}/{total} rows survived "
                         f"(< {min_kept_frac:.0%}) {rep['drop_reasons']}")
    return fails


def convert_dir(in_dir, out_dir, model_id=DEFAULT_MODEL, max_seq_length=2048,
                system=None, end_of_turn=END_OF_TURN, tokenizer=None):
    in_dir, out_dir = Path(in_dir), Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    tokenizer = tokenizer or load_serving_tokenizer(model_id)
    files = {}
    for name in ("train.jsonl", "valid.jsonl"):
        if (in_dir / name).is_file():
            files[name] = convert_file(tokenizer, in_dir / name, out_dir / name,
                                       max_seq_length, system=system, end_of_turn=end_of_turn)
    manifest = {
        "format": FORMAT_ID,
        "model": model_id,
        "template_sha256_16": tokenizer_fingerprint(tokenizer),
        "prompt_prefix": PRODUCTION_PREFIX,
        "prompt_suffix": PRODUCTION_SUFFIX,
        "end_of_turn": end_of_turn,
        "end_of_turn_id": tokenizer.convert_tokens_to_ids(end_of_turn),
        "enable_thinking": False,
        "system_prompt_sha256_16": (hashlib.sha256(system.encode()).hexdigest()[:16]
                                    if system else None),
        "system_prompt_tokens": (len(token_ids(tokenizer, system)) if system else 0),
        "max_seq_length": max_seq_length,
        "source_dir": str(in_dir),
        "files": files,
    }
    (out_dir / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in-dir", required=True, help="raw corpus dir (train.jsonl[, valid.jsonl])")
    ap.add_argument("--out-dir", required=True, help="templated corpus dir to write")
    ap.add_argument("--model", default=DEFAULT_MODEL, help="serving model id (tokenizer files only)")
    ap.add_argument("--max-seq-length", type=int, default=2048)
    ap.add_argument("--system-prompt-file", default=None,
                    help="system prompt for rows without their own `system` field")
    ap.add_argument("--end-of-turn", default=END_OF_TURN)
    ap.add_argument("--min-kept-frac", type=float, default=0.9,
                    help="fail if fewer than this fraction of train OR valid rows survive")
    args = ap.parse_args(argv)

    system = Path(args.system_prompt_file).read_text() if args.system_prompt_file else None
    m = convert_dir(args.in_dir, args.out_dir, model_id=args.model,
                    max_seq_length=args.max_seq_length, system=system,
                    end_of_turn=args.end_of_turn)
    print(f"[chat-template] system prompt: {m['system_prompt_tokens']} tokens"
          + ("" if system else " (none)"))
    for name, rep in m["files"].items():
        print(f"[chat-template] {name}: kept {rep['kept']}, dropped {rep['dropped']} "
              f"{rep['drop_reasons'] or ''} (max {rep['max_tokens']} tokens, prompt median "
              f"{rep['prompt_tokens_median']} max {rep['prompt_tokens_max']}, chosen reply "
              f"median share {rep['chosen_reply_share_median']})")
    print(f"[chat-template] suffix={m['prompt_suffix']!r} end_of_turn={m['end_of_turn']!r} "
          f"(id {m['end_of_turn_id']}) -> {args.out_dir}/{MANIFEST_NAME}")
    if "train.jsonl" not in m["files"]:
        print("[chat-template] FATAL: no train.jsonl", file=sys.stderr)
        return 1
    fails = kept_fraction_failures(m["files"], args.min_kept_frac)
    for f in fails:
        print(f"[chat-template] FATAL: {f}", file=sys.stderr)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
