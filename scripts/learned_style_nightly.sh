#!/usr/bin/env bash
# Nightly Learned Style Profile step: learner, then drift self-check.
# See docs/guides/learned-style.md.
#
# HU_LEARNED_STYLE_LEARN=0 makes this a no-op (default: on). Writing the
# learned file is harmless on its own: the C runtime only reads it when
# HU_LEARNED_STYLE is shadow or live.
#
# Both steps always run (`;` semantics): a learner refusal (exit 2) must not
# skip the drift reading. Exit status: the learner's if non-zero, otherwise
# the drift check's (1 = a contact flagged).
#
# Env: HU_PYTHON (interpreter; default /opt/homebrew/bin/python3, else python3),
#      HU_LEARNED_STYLE_PERSONA (default seth).
set -u

if [ "${HU_LEARNED_STYLE_LEARN:-1}" = "0" ]; then
  echo "[learned_style] HU_LEARNED_STYLE_LEARN=0: skipped"
  exit 0
fi

here="$(cd "$(dirname "$0")" && pwd)"
py="${HU_PYTHON:-}"
if [ -z "$py" ]; then
  if [ -x /opt/homebrew/bin/python3 ]; then py=/opt/homebrew/bin/python3; else py=python3; fi
fi
persona="${HU_LEARNED_STYLE_PERSONA:-seth}"

"$py" "$here/learned_style_profile.py" --persona "$persona"
learn=$?
"$py" "$here/learned_style_drift.py" --persona "$persona"
drift=$?
echo "[learned_style] learner_exit=$learn drift_exit=$drift"
if [ "$learn" -ne 0 ]; then
  exit "$learn"
fi
exit "$drift"
