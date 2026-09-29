#!/usr/bin/env python3
"""PreToolUse(Bash) guard: deny destructive commands that prefix deny-rules miss.

Permission rules like "Bash(git push --force*)" only match the literal start of
the command string, so `git -C . push --force`, `git push origin +main`,
`cd ~ && rm -rf .`, or `rm -rf "$HOME"` all slip past them. This hook tokenizes
the command with shlex, splits it into segments on shell operators, and checks
each segment's argv after resolving ~, $HOME, cd, and relative paths.

Blocks:
  - recursive rm of a protected target (see is_protected_rm_target)
  - git push with --force / --force-with-lease / -f / a "+refspec"
  - git reset --hard

Output contract: prints a PreToolUse "deny" decision as JSON and exits 0 when
blocking; exits 0 silently otherwise. Unparseable commands (e.g. unbalanced
quotes) are allowed through — the shell would reject them anyway.
"""

import json
import os
import shlex
import sys

HOME = os.path.expanduser("~")

SYSTEM_ROOTS = ("/Users", "/System", "/Library", "/Applications", "/usr", "/bin", "/sbin",
                "/etc", "/var", "/private", "/opt", "/opt/homebrew")
HOME_ROOTS = ("Documents", "Desktop", "Library", ".claude", ".human", ".config")
PROTECTED_SUBTREES = (".ssh", ".gnupg", ".human/personas", ".human/training-data",
                      ".human/adapters", ".human/adapter-current", ".human/memory.db",
                      ".human/cognition.db", ".human/graph.db", ".human/backups")

SEGMENT_OPERATORS = {";", "&&", "||", "|", "&", "\n"}
# Words that run the rest of the argv as a command.
COMMAND_PREFIXES = {"sudo", "command", "builtin", "exec", "nohup", "time", "nice", "env"}
SHELLS = {"sh", "bash", "zsh", "dash", "ksh"}
# git global options that consume the following token as their value.
GIT_OPTS_WITH_VALUE = {"-C", "-c", "--git-dir", "--work-tree", "--namespace", "--exec-path"}


def expand_home(token: str) -> str:
    for var in ("${HOME}", "$HOME"):
        token = token.replace(var, HOME)
    if token == "~" or token.startswith("~/"):
        token = HOME + token[1:]
    return token


def resolve_path(token: str, cwd: str) -> str:
    path = expand_home(token)
    # `rm -rf /*` or `rm -rf ~/*` empties the parent, so judge the parent.
    while path.endswith("/*") or path == "*":
        path = path[:-2] if path.endswith("/*") else "."
        if path == "":
            path = "/"
    if not os.path.isabs(path):
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


def split_segments(tokens):
    segment = []
    for tok in tokens:
        if tok in SEGMENT_OPERATORS:
            if segment:
                yield segment
            segment = []
        else:
            segment.append(tok)
    if segment:
        yield segment


def strip_prefixes(argv):
    """Drop leading VAR=val assignments and wrappers like sudo/env/nohup."""
    i = 0
    while i < len(argv):
        word = argv[i]
        if "=" in word and not word.startswith("-") and word.split("=", 1)[0].isidentifier():
            i += 1
        elif os.path.basename(word) in COMMAND_PREFIXES:
            i += 1
            # Skip the wrapper's own flags (sudo -u root, env -i, nice -n 10).
            while i < len(argv) and argv[i].startswith("-"):
                i += 1
        else:
            break
    return argv[i:]


def check_rm(args, cwd):
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
    for target in targets:
        resolved = resolve_path(target, cwd)
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


def check_command(command, cwd, depth=0):
    if depth > 3:
        return None
    try:
        lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|")
        lexer.whitespace_split = True
        lexer.commenters = "#"
        tokens = list(lexer)
    except ValueError:
        return None

    for segment in split_segments(tokens):
        argv = strip_prefixes(segment)
        if not argv:
            continue
        prog = os.path.basename(argv[0])
        if prog == "cd":
            cwd = resolve_path(argv[1] if len(argv) > 1 else "~", cwd)
            continue
        if prog in SHELLS and "-c" in argv[1:]:
            idx = argv.index("-c")
            if idx + 1 < len(argv):
                reason = check_command(argv[idx + 1], cwd, depth + 1)
                if reason:
                    return reason
            continue
        if prog == "eval":
            reason = check_command(" ".join(argv[1:]), cwd, depth + 1)
            if reason:
                return reason
            continue
        if prog == "rm":
            reason = check_rm(argv[1:], cwd)
        elif prog == "git":
            reason = check_git(argv[1:])
        else:
            reason = None
        if reason:
            return reason
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
