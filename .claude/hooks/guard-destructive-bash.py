#!/usr/bin/env python3
"""PreToolUse(Bash) guard: deny destructive commands that prefix deny-rules miss.

Permission rules like "Bash(git push --force*)" only match the literal start of
the command string, so `git -C . push --force`, `git push origin +main`,
`cd ~ && rm -rf .`, or `rm -rf "$HOME"` all slip past them. This hook tokenizes
the command with shlex, splits it into segments the way the shell would
(operators, newlines, subshells, compound-command keywords), unwraps command
wrappers, tracks cd and simple variable assignments, and checks each segment's
argv after expanding ~ and $VARS.

Blocks:
  - recursive rm of a protected target (see is_protected_rm_target), or of a
    target whose value cannot be known before execution ($(...), backticks,
    ${VAR:-default}, unset variables, xargs/stdin) — fail closed
  - git push with --force / --force-with-lease / -f / a "+refspec"
  - git reset --hard

Output contract: prints a PreToolUse "deny" decision as JSON and exits 0 when
blocking; exits 0 silently otherwise. Unparseable commands (e.g. unbalanced
quotes) are allowed through — the shell would reject them anyway.
"""

import json
import os
import re
import shlex
import sys

HOME = os.path.expanduser("~")

SYSTEM_ROOTS = ("/Users", "/System", "/Library", "/Applications", "/usr", "/bin", "/sbin",
                "/etc", "/var", "/private", "/opt", "/opt/homebrew")
HOME_ROOTS = ("Documents", "Desktop", "Library", ".claude", ".human", ".config")
PROTECTED_SUBTREES = (".ssh", ".gnupg", ".human/personas", ".human/training-data",
                      ".human/adapters", ".human/adapter-current", ".human/memory.db",
                      ".human/cognition.db", ".human/graph.db", ".human/backups")

PUNCTUATION = "();<>|&\n"
# Reserved words and grouping tokens that may precede a command in a segment.
KEYWORDS = {"{", "}", "!", "if", "then", "else", "elif", "fi", "do", "done", "while",
            "until", "case", "esac", "time"}
# Wrappers that run the rest of the argv as a command, with the flags that
# consume a following value and how many positional args precede the command.
WRAPPERS = {
    "sudo": ({"-u", "-g", "-h", "-p", "-C", "-D", "-r", "-t", "-U", "-T"}, 0),
    "env": ({"-u", "-C", "-S", "-P"}, 0),
    "nice": ({"-n"}, 0),
    "ionice": ({"-c", "-n", "-p"}, 0),
    "timeout": ({"-s", "-k", "--signal", "--kill-after"}, 1),
    "stdbuf": ({"-i", "-o", "-e"}, 0),
    "caffeinate": ({"-t", "-w"}, 0),
    "exec": ({"-a"}, 0),
    "command": (set(), 0),
    "builtin": (set(), 0),
    "nohup": (set(), 0),
}
XARGS_VALUE_FLAGS = {"-n", "-I", "-P", "-L", "-s", "-E", "-d", "-J", "-R"}
SHELLS = {"sh", "bash", "zsh", "dash", "ksh"}
# git global options that consume the following token as their value.
GIT_OPTS_WITH_VALUE = {"-C", "-c", "--git-dir", "--work-tree", "--namespace", "--exec-path"}

ASSIGNMENT = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", re.S)
VAR_REF = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)")


def expand(token, env):
    """Expand ~ and $VAR like the shell would; None if not knowable pre-execution."""
    if "$(" in token or "`" in token:
        return None
    if token.startswith("~"):
        token = os.path.expanduser(token)
        if token.startswith("~"):
            return None  # ~unknownuser

    missing = False

    def sub(m):
        nonlocal missing
        value = env.get(m.group(1) or m.group(2))
        if value is None:
            missing = True
            return ""
        return value

    token = VAR_REF.sub(sub, token)
    if missing or "$" in token:  # unset var, ${VAR:-x}, $1, $@ ...
        return None
    return token


def resolve_path(token, cwd, env):
    path = expand(token, env)
    if path is None:
        return None
    # `rm -rf /*` or `rm -rf ~/*` empties the parent, so judge the parent.
    while path.endswith("/*") or path == "*":
        path = path[:-2] if path.endswith("/*") else "."
        if path == "":
            path = "/"
    if not os.path.isabs(path):
        if cwd is None:
            return None
        path = os.path.join(cwd, path)
    return os.path.normpath(path) or "/"


def is_protected_rm_target(resolved: str) -> bool:
    """Return True if recursively deleting `resolved` must be blocked.

    `resolved` is an absolute, normalized path (no ~, no $HOME, no trailing
    slash, no `..`). HOME holds the user's home directory.
    """
    # Roots: deleting the directory itself is catastrophic, but deleting
    # inside it (build/, ~/.cache/foo) is routine work — exact match only.
    exact = {"/", HOME, *SYSTEM_ROOTS}
    exact.update(os.path.join(HOME, d) for d in HOME_ROOTS)
    project = os.environ.get("CLAUDE_PROJECT_DIR")
    if project:
        exact.add(os.path.normpath(project))
    if resolved in exact:
        return True
    # Irreplaceable data (persona identity, 71 GB of LoRA training data,
    # keys): deleting ANY part of these subtrees needs the user.
    for sub in PROTECTED_SUBTREES:
        root = os.path.join(HOME, sub)
        if resolved == root or resolved.startswith(root + os.sep):
            return True
    return False


def tokenize(command):
    lexer = shlex.shlex(command, posix=True, punctuation_chars=PUNCTUATION)
    lexer.whitespace = " \t\r"  # newline is a command separator, not whitespace
    lexer.whitespace_split = True
    lexer.commenters = ""  # `#` mid-word is literal; over-reading a real comment is fail-safe
    return list(lexer)


def heredoc_consumer_is_shell(tokens_before):
    """True if the command that reads a heredoc is a shell (so its body runs)."""
    segment = []
    for seg, _ in split_segments(tokens_before):
        segment = seg
    argv = strip_prefixes(segment, {})
    return bool(argv) and os.path.basename(argv[0]) in SHELLS and "-c" not in argv[1:]


def split_heredocs(command):
    """Return (command without heredoc bodies, [bodies that a shell will execute]).

    A heredoc body is stdin data unless a shell reads it. Operators are found
    with the same quote-aware tokenizer (so `echo "<<X"` is not a heredoc), and
    a body is removed only once its terminator line is actually found — an
    unterminated marker leaves every following line in place to be checked.
    """
    lines = command.split("\n")
    kept, shell_bodies = [], []
    i = 0
    while i < len(lines):
        line = lines[i]
        kept.append(line)
        i += 1
        try:
            tokens = tokenize(line)
        except ValueError:
            continue
        for t, tok in enumerate(tokens):
            if tok != "<<" or t + 1 >= len(tokens):
                continue  # `<<<` here-strings tokenize as "<<<" and are skipped
            delim, strip_tabs = tokens[t + 1], False
            if delim.startswith("-"):
                delim, strip_tabs = delim[1:], True
            end = next((j for j in range(i, len(lines))
                        if (lines[j].lstrip("\t") if strip_tabs else lines[j]) == delim), None)
            if end is None:
                continue
            body = "\n".join(lines[i:end])
            if heredoc_consumer_is_shell(tokens[:t]):
                shell_bodies.append(body)
            i = end + 1
    return "\n".join(kept), shell_bodies


def split_segments(tokens):
    """Yield (segment, depth_change) pairs, splitting where the shell would.

    shlex merges adjacent punctuation (`;(`, `);\\n`, `>&`), so separators are
    recognized by character class, not exact string. Redirections (`>`, `<`,
    `2>&`) do not split; they consume their target word.
    """
    segment = []
    skip_next = False
    for tok in tokens:
        if skip_next:
            skip_next = False
            continue
        if tok and all(c in PUNCTUATION for c in tok):
            if "<" in tok or ">" in tok:
                skip_next = True
                continue
            yield segment, tok.count("(") - tok.count(")")
            segment = []
        else:
            segment.append(tok)
    yield segment, 0


def strip_prefixes(argv, env):
    """Drop assignments, keywords and wrappers; return the argv actually executed.

    Assignments applied to a following command (`FOO=1 rm ...`) are dropped;
    a segment made only of assignments (`D=~`) updates `env` for later segments.
    """
    i = 0
    while i < len(argv):
        word = argv[i]
        m = ASSIGNMENT.match(word)
        if m:
            if all(ASSIGNMENT.match(w) for w in argv[i:]):
                for w in argv[i:]:
                    name, value = ASSIGNMENT.match(w).groups()
                    env[name] = expand(value, env)
                return []
            i += 1
        elif word in KEYWORDS or word == "export":
            i += 1
        elif os.path.basename(word) in WRAPPERS:
            value_flags, positionals = WRAPPERS[os.path.basename(word)]
            i += 1
            while i < len(argv) and argv[i].startswith("-") and argv[i] != "-":
                i += 2 if argv[i] in value_flags else 1
            i += positionals
        else:
            break
    return argv[i:]


def check_rm(args, cwd, env, from_stdin=False):
    recursive = False
    targets = []
    end_of_opts = False
    for arg in args:
        if not end_of_opts and arg == "--":
            end_of_opts = True
        elif not end_of_opts and arg.startswith("--"):
            recursive |= arg == "--recursive"
        elif not end_of_opts and arg.startswith("-") and len(arg) > 1:
            recursive |= "r" in arg or "R" in arg
        else:
            targets.append(arg)
    if not recursive:
        return None
    if from_stdin:
        return "recursive rm with targets from stdin (xargs) — cannot verify them"
    for target in targets:
        resolved = resolve_path(target, cwd, env)
        if resolved is None:
            return f"recursive rm of {target!r}, whose value is unknown until the shell runs"
        if is_protected_rm_target(resolved):
            return f"recursive rm of protected path {resolved!r} (from {target!r})"
    return None


def check_git(args):
    i = 0
    while i < len(args) and args[i].startswith("-"):
        opt = args[i]
        i += 2 if opt in GIT_OPTS_WITH_VALUE else 1
    if i >= len(args):
        return None
    sub, rest = args[i], args[i + 1:]
    if sub == "push":
        for arg in rest:
            if arg.startswith("--force"):  # --force, --force-with-lease, --force-if-includes
                return f"git push {arg}"
            if arg.startswith("-") and not arg.startswith("--") and "f" in arg[1:]:
                return f"git push {arg} (force)"
            if arg.startswith("+") or ":+" in arg:
                return f"git push {arg} (forced refspec)"
    if sub == "reset" and "--hard" in rest:
        return "git reset --hard"
    return None


def check_argv(argv, cwd, env, depth):
    prog = os.path.basename(argv[0])
    if prog in SHELLS and "-c" in argv[1:]:
        idx = argv.index("-c")
        return check_command(argv[idx + 1], cwd, depth + 1, env) if idx + 1 < len(argv) else None
    if prog == "eval":
        return check_command(" ".join(argv[1:]), cwd, depth + 1, env)
    if prog == "xargs":
        i = 1
        while i < len(argv) and argv[i].startswith("-"):
            i += 2 if argv[i] in XARGS_VALUE_FLAGS else 1
        inner = argv[i:]
        if inner and os.path.basename(inner[0]) == "rm":
            return check_rm(inner[1:], cwd, env, from_stdin=True)
        return check_argv(inner, cwd, env, depth) if inner else None
    if prog == "rm":
        return check_rm(argv[1:], cwd, env)
    if prog == "git":
        return check_git(argv[1:])
    return None


def check_command(command, cwd, depth=0, env=None):
    if depth > 3:
        return None
    if env is None:
        env = dict(os.environ)
        env["HOME"] = HOME
    command, shell_bodies = split_heredocs(command)
    for body in shell_bodies:
        reason = check_command(body, cwd, depth + 1, env)
        if reason:
            return reason
    try:
        tokens = tokenize(command)
    except ValueError:
        return None

    cwd_stack = []
    for segment, depth_change in split_segments(tokens):
        argv = strip_prefixes(segment, env)
        if argv:
            if os.path.basename(argv[0]) == "cd":
                cwd = resolve_path(argv[1] if len(argv) > 1 else "~", cwd, env)
            else:
                reason = check_argv(argv, cwd, env, depth)
                if reason:
                    return reason
        # Subshells: `( cd x && ... )` must not leak cwd past the `)`.
        for _ in range(max(depth_change, 0)):
            cwd_stack.append(cwd)
        for _ in range(max(-depth_change, 0)):
            if cwd_stack:
                cwd = cwd_stack.pop()
    return None


def main():
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    command = (payload.get("tool_input") or {}).get("command") or ""
    cwd = payload.get("cwd") or os.getcwd()
    reason = check_command(command, cwd)
    if reason:
        json.dump(
            {
                "hookSpecificOutput": {
                    "hookEventName": "PreToolUse",
                    "permissionDecision": "deny",
                    "permissionDecisionReason": f"guard-destructive-bash: blocked {reason}. "
                    "Ask the user to run this themselves if it is intended.",
                }
            },
            sys.stdout,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
