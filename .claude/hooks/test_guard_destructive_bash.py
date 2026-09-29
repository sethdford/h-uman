#!/usr/bin/env python3
"""Table test for guard-destructive-bash.py. Run: python3 .claude/hooks/test_guard_destructive_bash.py"""

import json
import os
import subprocess
import sys

HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "guard-destructive-bash.py")
HOME = os.path.expanduser("~")
REPO = "/tmp/repo"

BLOCK = [
    "rm -rf /",
    "rm -rf /*",
    "rm -fr ~",
    "rm -rf ~/",
    'rm -rf "$HOME"',
    "rm -rf ${HOME}/*",
    "rm -r -f /",
    "rm --recursive --force /",
    "cd ~ && rm -rf .",
    "cd / ; rm -rf *",
    "sudo rm -rf /",
    "FOO=1 rm -rf ~",
    "bash -c 'rm -rf ~'",
    "rm -rf /tmp/../",
    "git push --force",
    "git push -f origin main",
    "git push origin main -f",
    "git push -uf origin main",
    "git push --force-with-lease",
    "git push origin +main",
    "git push origin +HEAD:main",
    "git -C . push --force",
    "git -c core.editor=vi push -f",
    "git --no-pager reset --hard HEAD~1",
    "git reset --hard",
    "git status && git reset --hard origin/main",
    "echo hi; git push --force",
    "rm -rf ~/.human",
    "rm -rf ~/.human/training-data/adapters/old",
    "rm -rf ~/.human/personas/seth.json",
    "rm -rf /usr",
    "rm -rf ~/Documents",
    "rm -rf ~/.ssh/old_keys",
    "rm -rf /tmp/repo",
    "cd /tmp/repo/.. && rm -rf repo",
    "rm -rf $HOME/.human/memory.db",
]

ALLOW = [
    "ls -la",
    "rm -rf build",
    "rm -rf ./build-test",
    "rm -rf /tmp/scratch",
    "rm -rf ~/.cache/foo",
    "rm ~/notes.txt",
    "rm -f /",  # not recursive: rm refuses a directory without -r
    "git push",
    "git push -u origin feature",
    "git push origin main",
    "git reset --soft HEAD~1",
    "git reset HEAD file.c",
    "git log --format=%H -f",
    "echo 'git push --force'",
    "echo rm -rf /",
    "grep -r 'rm -rf /' docs/",
    "cmake --build build -j8",
    "rm -rf 'unbalanced",
    "rm -rf ~/.human/tmp",
    "rm -rf ~/Documents/scratch",
    "rm -rf /usr/local/tmp-build",
    "rm -rf /tmp/repo/build",
    "cd /tmp/repo && rm -rf build",
]


def decision(command: str):
    payload = json.dumps({"tool_name": "Bash", "tool_input": {"command": command}, "cwd": REPO})
    env = dict(os.environ, CLAUDE_PROJECT_DIR=REPO)
    out = subprocess.run([sys.executable, HOOK], input=payload, capture_output=True, text=True, check=True,
                         env=env).stdout
    return json.loads(out)["hookSpecificOutput"]["permissionDecision"] if out.strip() else "allow"


def main():
    failures = []
    for cmd in BLOCK:
        if decision(cmd) != "deny":
            failures.append(f"expected DENY : {cmd}")
    for cmd in ALLOW:
        if decision(cmd) != "allow":
            failures.append(f"expected ALLOW: {cmd}")
    total = len(BLOCK) + len(ALLOW)
    for f in failures:
        print(f)
    print(f"Results: {total - len(failures)}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
