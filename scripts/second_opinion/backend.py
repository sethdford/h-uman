# scripts/second_opinion/backend.py
"""Model access for the second-opinion lane (spec §3, §5).

Local Gemma is the default and never leaves the Mac. Vertex is opt-in only:
every job here reads real message text, and the blind-A/B rule is "never send
real messages to a cloud judge" unless Seth explicitly asks (--backend vertex).
"""
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from contextlib import contextmanager

GEMMA_MODEL = "mlx-community/gemma-4-31b-it-4bit"
GEMMA_PORT = 8743
GEMMA_PYTHON = os.path.expanduser("~/Documents/gemma-realtime-1/.venv312/bin/python")
VERTEX_MODEL = "gemini-3.8-flash"
VERTEX_PROJECT = "johnb-2025"
ADC_PATH = os.path.expanduser("~/.config/gcloud/application_default_credentials.json")
LOOPBACK = {"127.0.0.1", "localhost", "::1"}


class BackendError(RuntimeError):
    pass


def _post_json(url, body, headers, timeout):
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", **headers})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except (urllib.error.URLError, OSError, ValueError) as e:
        # No URL, headers or body in the message: headers can carry a bearer token.
        raise BackendError(f"request failed: {type(e).__name__}") from e


def _nonempty(text, who):
    """An empty or missing reply is a failure, never an answer: callers count it
    as an error instead of storing a verdict nobody gave."""
    if not isinstance(text, str) or not text.strip():
        raise BackendError(f"{who} returned no content")
    return text


class GemmaBackend:
    def __init__(self, base_url=f"http://127.0.0.1:{GEMMA_PORT}", model=GEMMA_MODEL, timeout=300,
                 post=_post_json):
        if urllib.parse.urlparse(base_url).hostname not in LOOPBACK:
            raise BackendError(f"refusing non-local Gemma URL: {base_url}")
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout = timeout
        self._post = post
        self.name = f"{model.rsplit('/', 1)[-1]}@local"

    def generate(self, system, user, max_tokens=400):
        # Gemma 4's chat template does support a system role; the system prompt is
        # folded into the user turn anyway, for simplicity and consistency.
        # Thinking off: Gemma 4 reasons first by default, and on the audit's short
        # budgets the reasoning used every token and left content empty (every audit
        # errored in the 2026-09-30 smoke test). The judge, which runs through
        # synthetic_judge.py, budgets for thinking instead (max_tokens 1024).
        body = {"model": self.model, "max_tokens": max_tokens, "temperature": 0.0,
                "chat_template_kwargs": {"enable_thinking": False},
                "messages": [{"role": "user", "content": f"{system}\n\n{user}"}]}
        d = self._post(self.base_url + "/v1/chat/completions", body, {}, self.timeout)
        try:
            content = d["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as e:
            raise BackendError(f"unexpected Gemma response shape: {e}") from e
        return _nonempty(content, "Gemma")


def _adc_token(creds_path=ADC_PATH):
    if not os.path.exists(creds_path):
        raise BackendError("no ADC credentials; run `gcloud auth application-default login`")
    try:
        with open(creds_path) as f:
            creds = json.load(f)
        payload = urllib.parse.urlencode({
            "client_id": creds["client_id"], "client_secret": creds["client_secret"],
            "refresh_token": creds["refresh_token"], "grant_type": "refresh_token"}).encode()
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise BackendError(f"unreadable ADC credentials ({type(e).__name__})") from e
    req = urllib.request.Request("https://oauth2.googleapis.com/token", data=payload,
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())["access_token"]
    except (urllib.error.URLError, OSError, ValueError, KeyError) as e:
        raise BackendError(f"ADC token refresh failed ({type(e).__name__})") from e


class VertexBackend:
    def __init__(self, allow=False, model=VERTEX_MODEL, project=VERTEX_PROJECT, thinking_budget=0,
                 timeout=120, post=_post_json, token=_adc_token, notice=sys.stderr):
        if not allow:
            raise BackendError("the Vertex backend is opt-in: pass --backend vertex explicitly")
        print(f"second-opinion: backend=vertex; message excerpts will leave this Mac "
              f"(Google Cloud project {project})", file=notice)
        self.model = model
        self.project = project
        self.thinking_budget = thinking_budget
        self.timeout = timeout
        self._post = post
        self._token = token
        self.name = f"{model}@vertex"

    def generate(self, system, user, max_tokens=400):
        url = (f"https://aiplatform.googleapis.com/v1/projects/{self.project}/locations/global/"
               f"publishers/google/models/{self.model}:generateContent")
        body = {"systemInstruction": {"parts": [{"text": system}]},
                "contents": [{"role": "user", "parts": [{"text": user}]}],
                "generationConfig": {"temperature": 0.0, "maxOutputTokens": max_tokens,
                                     "thinkingConfig": {"thinkingBudget": self.thinking_budget}}}
        d = self._post(url, body, {"Authorization": f"Bearer {self._token()}"}, self.timeout)
        try:
            text = "".join(p.get("text", "") for p in d["candidates"][0]["content"]["parts"])
        except (KeyError, IndexError, TypeError, AttributeError) as e:
            raise BackendError(f"unexpected Vertex response shape: {e}") from e
        return _nonempty(text, "Vertex")


def _healthy(url):
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return r.status == 200
    except Exception:
        return False


@contextmanager
def serve_gemma(model=GEMMA_MODEL, port=GEMMA_PORT, python=GEMMA_PYTHON, wait_s=600,
                popen=subprocess.Popen, health=None, sleep=time.sleep, clock=time.monotonic,
                log_path=None):
    """Start mlx_lm.server on 127.0.0.1:<port>, yield a GemmaBackend once healthy, and
    always stop it. Refuses a port that is already serving: it may be another
    session's spare server (.claude/rules/session-worktree-isolation.md)."""
    if health is None:
        health = lambda: _healthy(f"http://127.0.0.1:{port}/health")  # noqa: E731
    if health():
        raise BackendError(f"port {port} is already serving; refusing to share or kill it")
    cmd = [python, "-m", "mlx_lm.server", "--model", model, "--host", "127.0.0.1",
           "--port", str(port)]
    if log_path:
        os.makedirs(os.path.dirname(log_path) or ".", exist_ok=True)
    log = open(log_path, "ab") if log_path else subprocess.DEVNULL
    proc = popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = clock() + wait_s
        while not health():
            if proc.poll() is not None:
                raise BackendError(f"Gemma server exited before becoming healthy "
                                   f"(rc={proc.returncode})")
            if clock() >= deadline:
                raise BackendError(f"Gemma server not healthy after {wait_s}s")
            sleep(2)
        yield GemmaBackend(f"http://127.0.0.1:{port}", model)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if log_path:
            log.close()
