#!/usr/bin/env python3
"""LongMemEval QA-accuracy harness: retrieve context, generate answers, judge with LLM.

Measures QA accuracy on LongMemEval-S using:
1. Haystack ingestion into a scratch memory.db
2. Retrieval via `human memory search --hybrid --plain` (production path)
3. Answer generation with Gemini
4. Judgment with Gemini using official LongMemEval judge prompts
5. Checkpoint to JSONL for resumability

Result structure: per-question {question_id, question_type, verdict, answer, error}
  verdict: "correct" | "incorrect" | "error" (third only if LLM or retrieval fails)

Final summary: n_attempted, n_scored, n_errors, accuracy overall and per_question_type.
Refuses to print accuracy if errors > 2% of attempted.

Run (full 500, ~resumable; uses the :8741 embedder that also serves live
replies, so run it overnight):
    python3 scripts/eval_longmemeval_qa.py --bin ./build/human --resume \
        --out-dir ~/.human/benchmarks/longmemeval/runs/<name>
Dataset: --data (default ~/.human/benchmarks/longmemeval/longmemeval_s_cleaned.json,
from huggingface.co/datasets/xiaowu0162/longmemeval-cleaned). Published systems
mostly judge with GPT-4o; this judges with Gemini, so compare with that caveat.
"""
import argparse, collections, json, os, random, re, shutil, sqlite3, subprocess, sys, time, urllib.request, uuid
import dataclasses

# Reuse ingestion helpers from eval_memory_benchmarks.py
sys.path.insert(0, os.path.dirname(__file__))
from eval_memory_benchmarks import build_db_retry, sh, embed_env, parse_keys, plain_env

PROJECT_ID = os.environ.get("GCP_PROJECT", "johnb-2025")
EVAL_MODEL_ANSWER = os.environ.get("EVAL_MODEL_ANSWER", "gemini-3.1-pro-preview")
EVAL_MODEL_JUDGE = os.environ.get("EVAL_MODEL_JUDGE", "gemini-3.1-pro-preview")

_adc_token_cache = {}

def _get_adc_token():
    """Fetch Application Default Credentials bearer token via gcloud."""
    if _adc_token_cache.get("token") and time.time() < _adc_token_cache.get("expires", 0):
        return _adc_token_cache["token"]
    try:
        # Use gcloud to get the token (most reliable for ADC)
        import subprocess
        result = subprocess.run(
            ["gcloud", "auth", "application-default", "print-access-token"],
            capture_output=True, text=True, timeout=10
        )
        if result.returncode == 0:
            token = result.stdout.strip()
            _adc_token_cache["token"] = token
            _adc_token_cache["expires"] = time.time() + 3600
            return token
    except Exception as e:
        pass

    # Fallback: try reading ADC JSON directly
    try:
        cred_file = os.path.expanduser("~/.config/gcloud/application_default_credentials.json")
        cred = json.load(open(cred_file))
        if "refresh_token" in cred:
            payload = urllib.parse.urlencode({
                "client_id": cred.get("client_id", ""),
                "client_secret": cred.get("client_secret", ""),
                "refresh_token": cred["refresh_token"],
                "grant_type": "refresh_token",
            }).encode()
            req = urllib.request.Request("https://oauth2.googleapis.com/token",
                                         data=payload, headers={"Content-Type": "application/x-www-form-urlencoded"})
            resp = urllib.request.urlopen(req, timeout=10)
            data = json.loads(resp.read())
            _adc_token_cache["token"] = data["access_token"]
            _adc_token_cache["expires"] = time.time() + data.get("expires_in", 3600)
            return data["access_token"]
    except Exception:
        pass

    return None

def _vertex_url(model):
    """Vertex AI Gemini endpoint (ADC bearer only: CLAUDE.md, a ?key= call 401s)."""
    # Never write "$base/$model:generateContent" in zsh without braces; history expansion breaks it
    return (f"https://aiplatform.googleapis.com/v1/projects/{PROJECT_ID}/locations/global/"
            f"publishers/google/models/{model}:generateContent")

def call_gemini(prompt, model, thinking_budget=0, max_tokens=2048):
    """Call Gemini via Vertex AI with Application Default Credentials.

    Args:
        prompt: text prompt
        model: model ID (e.g. "gemini-3.1-pro-preview")
        thinking_budget: tokens for thinking (0=disabled, >0=enabled). Gemini 3.x shares
                         maxOutputTokens between thinking and reply, so thinking_budget is
                         deducted from max_tokens.
        max_tokens: max reply tokens (includes thinking if thinking_budget > 0)

    Returns: reply text, or raises on API error.
    """
    gen_cfg = {
        "temperature": 0.0,  # deterministic answers and verdicts, as in LongMemEval
        "maxOutputTokens": max_tokens,
    }
    if thinking_budget > 0:
        gen_cfg["thinkingConfig"] = {"thinkingBudget": thinking_budget}
    else:
        gen_cfg["thinkingConfig"] = {"thinkingBudget": 0}

    payload = json.dumps({
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": gen_cfg,
    }).encode()

    token = _get_adc_token()
    if not token:
        raise RuntimeError("no ADC token (gcloud auth application-default login)")
    headers = {"Content-Type": "application/json", "Authorization": f"Bearer {token}"}

    req = urllib.request.Request(_vertex_url(model), data=payload, headers=headers)
    resp = urllib.request.urlopen(req, timeout=120)
    data = json.loads(resp.read())

    # Extract text, skipping thinking block if present
    candidates = data.get("candidates", [])
    if not candidates:
        raise RuntimeError(f"No candidates in Gemini response: {data}")

    parts = candidates[0].get("content", {}).get("parts", [])
    for part in parts:
        if "text" in part:
            return part["text"]

    # If no text part, check if we hit token limit with only thinking
    finish_reason = candidates[0].get("finishReason", "")
    if finish_reason == "MAX_TOKENS" and all("text" not in p for p in parts):
        raise RuntimeError(f"Gemini hit MAX_TOKENS with no output (thinking exhausted the budget)")

    raise RuntimeError(f"No text in Gemini response: {data}")

@dataclasses.dataclass
class QAResult:
    """One question's result."""
    question_id: str
    question_type: str
    verdict: str  # "correct" | "incorrect" | "error"
    error: str = None
    answer: str = None
    retrieved_count: int = 0

    def to_dict(self):
        return dataclasses.asdict(self)

# Verbatim from LongMemEval src/evaluation/evaluate_qa.py, get_anscheck_prompt
# (github.com/xiaowu0162/LongMemEval, fetched 2026-10-01); only the positional
# "{}" slots are named here.
_JUDGE_ALL = (
    "I will give you a question, a correct answer, and a response from a model. Please answer "
    "yes if the response contains the correct answer. Otherwise, answer no. If the response is "
    "equivalent to the correct answer or contains all the intermediate steps to get the correct "
    "answer, you should also answer yes. If the response only contains a subset of the "
    "information required by the answer, answer no. ")
_JUDGE_TAIL = ("\n\nQuestion: {question}\n\nCorrect Answer: {answer}\n\nModel Response: {response}"
               "\n\nIs the model response correct? Answer yes or no only.")
JUDGE_TEMPLATES = {
    "single-session-user": _JUDGE_ALL + _JUDGE_TAIL,
    "single-session-assistant": _JUDGE_ALL + _JUDGE_TAIL,
    "multi-session": _JUDGE_ALL + _JUDGE_TAIL,
    "temporal-reasoning": _JUDGE_ALL + (
        "In addition, do not penalize off-by-one errors for the number of days. If the question "
        "asks for the number of days/weeks/months, etc., and the model makes off-by-one errors "
        "(e.g., predicting 19 days when the answer is 18), the model's response is still correct. "
    ) + _JUDGE_TAIL,
    "knowledge-update": (
        "I will give you a question, a correct answer, and a response from a model. Please answer "
        "yes if the response contains the correct answer. Otherwise, answer no. If the response "
        "contains some previous information along with an updated answer, the response should be "
        "considered as correct as long as the updated answer is the required answer."
    ) + _JUDGE_TAIL,
    "single-session-preference": (
        "I will give you a question, a rubric for desired personalized response, and a response "
        "from a model. Please answer yes if the response satisfies the desired response. "
        "Otherwise, answer no. The model does not need to reflect all the points in the rubric. "
        "The response is correct as long as it recalls and utilizes the user's personal "
        "information correctly.\n\nQuestion: {question}\n\nRubric: {answer}\n\nModel Response: "
        "{response}\n\nIs the model response correct? Answer yes or no only."),
}
JUDGE_ABSTENTION = (
    "I will give you an unanswerable question, an explanation, and a response from a model. "
    "Please answer yes if the model correctly identifies the question as unanswerable. The model "
    "could say that the information is incomplete, or some other information is given but the "
    "asked information is not.\n\nQuestion: {question}\n\nExplanation: {answer}\n\nModel "
    "Response: {response}\n\nDoes the model correctly identify the question as unanswerable? "
    "Answer yes or no only.")


def judge_prompt_for_type(question_type, question_id=""):
    """The official judge template. Abstention is a property of the question
    (its id ends in "_abs"), not a question_type. Unknown types raise, as the
    official code does, rather than being judged by an invented prompt."""
    if question_id.endswith("_abs"):
        return JUDGE_ABSTENTION
    return JUDGE_TEMPLATES[question_type]


def answer_from_retrieved(question, retrieved_turns, question_date):
    """Prompt for the answer model: the retrieved turns (each with its session
    date, oldest first, so "how many days between" questions read in order) and
    the date the question is asked. The gold answer never enters this prompt."""
    context = ""
    if retrieved_turns:
        context = "## Retrieved Context\n\n"
        for i, (turn_text, sess_date) in enumerate(retrieved_turns, 1):
            context += f"[{i}] ({sess_date}) {turn_text}\n\n"
    return (
        f"Question: {question}\n\n"
        f"Question Date: {question_date}\n\n"
        f"{context}"
        f"Please answer the question based on the retrieved context above. "
        f"Keep your answer concise (1-2 sentences). "
        f"If the context does not contain enough information to answer, say so."
    )

# Production fusion (~/Library/LaunchAgents/ai.human.service-loop.plist,
# HU_HYBRID_FUSION=score, HU_HYBRID_FUSION_ALPHA=0.80 as of 2026-10-01).
PROD_FUSION = ("score", 0.80)


def session_of(key):
    """'s<session_id>:t<turn>' -> session_id (ids may themselves contain ':')."""
    return key[1:].rsplit(":t", 1)[0] if key.startswith("s") and ":t" in key else None


def turn_of(key):
    try:
        return int(key.rsplit(":t", 1)[1])
    except (IndexError, ValueError):
        return 0


def order_turns(keys, text_by_key, session_dates):
    """Retrieved keys -> [(text, session date)] in time order; unknown keys dropped."""
    picked = [k for k in keys if k in text_by_key]
    picked.sort(key=lambda k: (session_dates.get(session_of(k), ""), session_of(k) or "", turn_of(k)))
    return [(text_by_key[k], session_dates.get(session_of(k), "unknown")) for k in picked]


def retrieve_context(binp, dbp, question, text_by_key, session_dates, k=10):
    """Top-k turns through `human memory search --hybrid --plain` (the daemon
    loader's path). Output lines are "[N] key (score): text"; keys are parsed
    with eval_memory_benchmarks.parse_keys and the text comes from the rows we
    ingested, not from the (truncatable) display line."""
    env = {**embed_env(), **plain_env(*PROD_FUSION)}
    keys = parse_keys(sh(binp, dbp, ["search", "--hybrid", "--plain", question], env))[:k]
    return order_turns(keys, text_by_key, session_dates)


def load_questions(path, limit=None, types=None, seed=3):
    """All LongMemEval questions (optionally a seeded sample of `limit`), as
    (question, rows) with rows = (key, session_id, "role: content")."""
    data = json.load(open(path))
    if types:
        data = [q for q in data if q["question_type"] in types]
    if limit and limit < len(data):
        rng = random.Random(seed)
        data = rng.sample(data, limit)
    out = []
    for q in data:
        rows = []
        for sid, sess in zip(q["haystack_session_ids"], q["haystack_sessions"]):
            for t, turn in enumerate(sess):
                rows.append((f"s{sid}:t{t}", str(sid), f"{turn['role']}: {turn['content']}"))
        out.append((q, rows))
    return out


def process_question(binp, q, rows, out_dir, k=10, answer_model=EVAL_MODEL_ANSWER, judge_model=EVAL_MODEL_JUDGE):
    """Process one question: ingest, retrieve, answer, judge.

    Returns: QAResult
    """
    question_id = q["question_id"]
    question_type = q["question_type"]
    question_text = q["question"]
    correct_answer = q["answer"]
    question_date = q["question_date"]

    # Build scratch db path
    dbp = os.path.join(out_dir, f"qa_{question_id}.db")

    try:
        # Build session_id to date mapping from the dataset
        # haystack_session_ids and haystack_dates are parallel lists, both per-session
        session_dates = {}
        for sid, date in zip(q.get("haystack_session_ids", []), q.get("haystack_dates", [])):
            session_dates[sid] = date

        # Ingest haystack
        idx = build_db_retry(binp, dbp, rows)
        if idx < len(rows) * 0.9:
            return QAResult(question_id, question_type, "error",
                          error=f"indexed {idx}/{len(rows)} rows (<90%)")

        # Retrieve context
        text_by_key = {key: text for key, _sid, text in rows}
        retrieved = retrieve_context(binp, dbp, question_text, text_by_key, session_dates, k=k)
        if not retrieved:
            return QAResult(question_id, question_type, "error",
                          error="retrieval returned no results", retrieved_count=0)

        # Generate answer
        answer_prompt = answer_from_retrieved(question_text, retrieved, question_date)
        try:
            generated_answer = call_gemini(answer_prompt, answer_model, thinking_budget=512, max_tokens=2048)
        except Exception as e:
            return QAResult(question_id, question_type, "error",
                          error=f"answer model failed: {e}", retrieved_count=len(retrieved))

        # Judge answer
        judge_template = judge_prompt_for_type(question_type, question_id)
        judge_prompt_text = judge_template.format(
            question=question_text,
            answer=correct_answer,
            response=generated_answer
        )

        try:
            judgment = call_gemini(judge_prompt_text, judge_model, thinking_budget=0, max_tokens=512)
            # Extract yes/no from response
            judgment_lower = judgment.lower().strip()
            if "yes" in judgment_lower:
                verdict = "correct"
            elif "no" in judgment_lower:
                verdict = "incorrect"
            else:
                # Ambiguous response; treat as error
                return QAResult(question_id, question_type, "error",
                              error=f"judge response ambiguous: {judgment}",
                              answer=generated_answer, retrieved_count=len(retrieved))
        except Exception as e:
            return QAResult(question_id, question_type, "error",
                          error=f"judge model failed: {e}",
                          answer=generated_answer, retrieved_count=len(retrieved))

        return QAResult(question_id, question_type, verdict,
                       answer=generated_answer, retrieved_count=len(retrieved))

    except Exception as e:
        return QAResult(question_id, question_type, "error", error=str(e))

    finally:
        # Clean up scratch db
        for s in ("", "-wal", "-shm"):
            try:
                os.remove(dbp + s)
            except FileNotFoundError:
                pass

def load_checkpoint(checkpoint_path):
    """Load results from JSONL checkpoint.

    Returns: {question_id: QAResult, ...}
    """
    results = {}
    if not os.path.exists(checkpoint_path):
        return results

    with open(checkpoint_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                data = json.loads(line)
                qid = data["question_id"]
                results[qid] = QAResult(**data)
            except Exception as e:
                print(f"Warning: failed to parse checkpoint line: {e}", file=sys.stderr)

    return results

def save_result(checkpoint_path, result):
    """Append result to JSONL checkpoint."""
    with open(checkpoint_path, "a") as f:
        f.write(json.dumps(result.to_dict()) + "\n")

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", default="./build/human", help="Path to h-uman binary")
    ap.add_argument("--limit", type=int, default=500, help="Max questions to run (default 500, full dataset)")
    ap.add_argument("--types", nargs="+", help="Filter by question_type (e.g. temporal-reasoning single-session-user)")
    ap.add_argument("--k", type=int, default=10, help="Retrieve top-k turns (default 10)")
    ap.add_argument("--answer-model", default=EVAL_MODEL_ANSWER, help="Answer generation model")
    ap.add_argument("--judge-model", default=EVAL_MODEL_JUDGE, help="Answer judgment model")
    ap.add_argument("--out-dir", default=None, help="Output directory (default ~/.human/benchmarks/longmemeval/runs/<date>)")
    ap.add_argument("--resume", action="store_true", help="Resume from checkpoint if exists")
    ap.add_argument("--data", default=os.path.expanduser(
        "~/.human/benchmarks/longmemeval/longmemeval_s_cleaned.json"), help="LongMemEval-S json")

    a = ap.parse_args()

    # Setup output directory
    if not a.out_dir:
        base = os.path.expanduser("~/.human/benchmarks/longmemeval/runs")
        a.out_dir = os.path.join(base, time.strftime("%Y-%m-%d_%H-%M-%S"))
    os.makedirs(a.out_dir, exist_ok=True)

    checkpoint_path = os.path.join(a.out_dir, "results.jsonl")
    summary_path = os.path.join(a.out_dir, "summary.json")

    # Load dataset
    # The embedder the daemon itself uses; eval_memory_benchmarks defaults to a
    # :8749 server that is not running.
    os.environ.setdefault("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8741")
    qs = load_questions(a.data, a.limit, a.types)

    print(f"Running {len(qs)} questions, output: {a.out_dir}", flush=True)

    # Load checkpoint if resuming
    done = load_checkpoint(checkpoint_path) if a.resume else {}

    # Process questions
    results = list(done.values())
    for n, (q, rows) in enumerate(qs, 1):
        qid = q["question_id"]

        if qid in done:
            print(f"[{n}/{len(qs)}] {qid} SKIPPED (already done)", flush=True)
            continue

        result = process_question(a.bin, q, rows, a.out_dir, k=a.k,
                                 answer_model=a.answer_model, judge_model=a.judge_model)
        results.append(result)
        save_result(checkpoint_path, result)

        print(f"[{n}/{len(qs)}] {q['question_id']} ({q['question_type'][:20]:20}) "
              f"verdict={result.verdict:10} retrieved={result.retrieved_count} error={result.error or ''}", flush=True)

    # Summarize
    by_type = collections.defaultdict(list)
    n_correct = 0
    n_scored = 0
    n_errors = 0

    for r in results:
        by_type[r.question_type].append(r)
        if r.verdict == "error":
            n_errors += 1
        else:
            n_scored += 1
            if r.verdict == "correct":
                n_correct += 1

    n_attempted = len(results)
    error_pct = n_errors / n_attempted if n_attempted > 0 else 0

    print(f"\n=== SUMMARY ===", flush=True)
    print(f"n_attempted: {n_attempted}", flush=True)
    print(f"n_scored: {n_scored}", flush=True)
    print(f"n_errors: {n_errors}", flush=True)
    print(f"error_rate: {error_pct:.1%}", flush=True)

    if error_pct > 0.02:
        print(f"REFUSING: error rate {error_pct:.1%} > 2%", flush=True)
        sys.exit(2)

    if n_scored > 0:
        accuracy = n_correct / n_scored
        print(f"accuracy: {accuracy:.3f} ({n_correct}/{n_scored})", flush=True)
        print("\nby question_type:", flush=True)
        for qtype in sorted(by_type.keys()):
            rs = by_type[qtype]
            correct = sum(1 for r in rs if r.verdict == "correct")
            scored = sum(1 for r in rs if r.verdict != "error")
            if scored > 0:
                acc = correct / scored
                print(f"  {qtype:30} {acc:.3f} ({correct}/{scored})", flush=True)

        summary = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "n_attempted": n_attempted,
            "n_scored": n_scored,
            "n_errors": n_errors,
            "accuracy": accuracy,
            "by_type": {
                qtype: {
                    "accuracy": sum(1 for r in rs if r.verdict == "correct") / sum(1 for r in rs if r.verdict != "error"),
                    "n": len(rs),
                }
                for qtype, rs in by_type.items()
                if sum(1 for r in rs if r.verdict != "error") > 0
            }
        }
        json.dump(summary, open(summary_path, "w"), indent=2)
        print(f"\nwrote {summary_path}", flush=True)

if __name__ == "__main__":
    sys.exit(main())
