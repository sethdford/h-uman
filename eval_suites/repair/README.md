# Other-initiated repair scenario pack (B13 smoke)

Text-only repair prompts with expected **dialog** and **relational** act labels
(string names matching `hu_dialog_act_name` / `hu_relational_act_name`).

Used by `tests/test_behavior_corpora.c` for structure + heuristic alignment
smoke checks. Not loaded by `human eval validate` (lives outside top-level
`eval_suites/*.json` task files).
