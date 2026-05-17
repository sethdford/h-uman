# Sprint 7 — Decision Log

Stakeholder: Seth Ford (PO + technical lead)
Date: 2026-05-16

## D1 — US-7.1 DPO path: adopt `mlx-lm-lora` fork

**Decision:** Pin `mlx-lm-lora>=2.1.0,<3` (PyPI: `mlx-lm-lora`, repo `Goekdeniz-Guelmez/mlx-lm-lora`).

**Why:** Stock `mlx_lm` 0.31.2 and upstream `main` have no DPO support; PR #794 closed unmerged, PR #417 still open. The fork is a verified drop-in superset. Story stays M-sized.

**AC revisions:**
- **AC-7.1.1** literal `--fine-tune-type dpo` is replaced with `-m mlx_lm_lora.train --train-mode dpo --train-type lora`. The argv-shape assertion in `tests/test_finetune_gemma_dpo.py` checks for these tokens, not the original string.

**Risk acknowledged:** Single-maintainer third-party dependency. Mitigation: pin the major version; add a CI smoke test that the wheel installs cleanly; document the upgrade path if upstream `mlx_lm` ever lands DPO natively.

**Implementer note:** `--reference-model-path` MUST be set explicitly to the base model — otherwise `mlx-lm-lora` defaults `reference = policy` after `--resume-adapter-file`, producing near-zero gradient on step 1 (pre-flighted in design §5.3).

---

## D2 — US-7.2 mining path: reuse `chat.db` correction signal

**Decision:** Option (b) — new `src/ml/dpo_miner.c` + `human ml mine-corrections` subcommand. Reuses the existing user-correction signal already mined by `hu_training_data_extract_dpo` (pattern `user(N) → assistant(N+1) → user(N+2)` within `HU_DPO_CORRECTION_WINDOW_SEC=300`). Adds PII redaction via `hu_pii_redact` + content-hash dedup. Tags `source='outbound_edit'`.

**Why:** `13b89763`'s outbound-dedup is an in-memory ring, not a SQLite table — the literal AC signal does not exist on disk yet, and persisting it would require touching the imessage hot path AND adding a draft-review hook (L-sized, multi-week, HIGH risk). The chat.db signal is the realistic, available data source for this sprint.

**AC revisions:**
- **AC-7.2.1** "outbound-dedup table contains a row where draft_text != sent_text" becomes "chat.db contains a user-correction triple `user → assistant → user` within the correction window, and the agent's assistant response was sent to the channel" — verified by deterministic in-memory SQLite fixture in `tests/test_dpo_miner.c`.
- **AC-7.2.4** PII function: `hu_personal_model_redact_pii` → `hu_pii_redact` (the function that actually exists).
- The standalone miner adds `hu_pii_redact` pass + content-hash dedup that the existing `hu_training_data_extract_dpo` lacks; these are the value-adds the new module justifies.

**Out of scope (acknowledged):** No new SQLite draft/sent table this sprint; no imessage hot-path changes; no draft-review hook. A future story can add real draft-vs-sent persistence once the channel pipeline has a place to put it.

---

## D3 — US-7.6 judgment-fidelity: ship seam dormant

**Decision:** Land `hu_ml_nll_compute_fn_t` function-pointer typedef + mock test infrastructure. Production default returns `HU_ERR_NOT_SUPPORTED`. `check-lora-ab.sh --judgment` emits visible `SKIP` line so US-7.5 nightly cron cannot silently pass an inactive gate. Follow-on **US-7.6.1 (deferred to Sprint 8)** wires `src/ml/gpt.c` as the real NLL backend.

**Why:** Wiring reference GPT now expands story to L and adds a "scored against toy model" caveat. The seam-first approach is honest, ships the gate-script changes that protect US-7.5, and queues the real work cleanly.

**AC revisions:** None to the listed AC. The implementer must also ensure the `SKIP` output in `check-lora-ab.sh --judgment` is parseable as a non-pass — so a downstream automation can't accidentally treat it as success.

---

## D4 — US-7.3 honesty-gate: per-process one-shot

**Decision:** `static int s_personalization_warn_emitted = 0` fires the warning once per daemon process. `HU_IS_TEST` shim resets it for unit tests.

**Why:** Matches existing one-shot patterns in `src/daemon.c`. Avoids spamming the operator on config reloads. Persisting suppression to config is YAGNI for a low-traffic warning.

**AC revisions:** None. Implementer must add a comment at the one-shot site noting the reset shim is test-only.

---

## Cross-cutting note — Wave 0 surfaced two AC-vs-code drifts

Both US-7.1 and US-7.2 had AC built on assumptions that did not match the codebase. The tech-lead pass caught them before any implementation. This is the adversarial grounding pattern working as intended — the cost of these 4 tech-lead invocations was paid back many times over by NOT spawning an implementer to chase a non-existent SQLite schema or a missing mlx_lm flag.

**Retro candidate:** When the PO authors AC referencing a recent commit's data shape, the PO should `git show <sha>` rather than infer from the commit message. Both this sprint's surfaced drifts could have been caught one phase earlier.
