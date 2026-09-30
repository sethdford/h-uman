# F4: every config write goes through the mutator

Written 2026-09-29 against `origin/main` `ef6417bf1`. Closes follow-up F4 from
`2026-09-28-dead-code-decisions.md` ("route config writes through
config_mutator").

## What was measured

F4 was filed as "the mutator is unwired". Measuring it found three live bugs
behind that:

| Writer | Path | What it did |
|---|---|---|
| `human workspace set <dir>` | `hu_config_save` | Rebuilt `config.json` from `hu_config_t`. The serializer emits **33 of the 57** top-level keys the parser reads, so every other section was erased: `initiative`, `reaction_collection`, `router`, `voice`, `policy`, `behavior`, `feeds`, `follow_up_watcher`, `reflection`, `plugins`, … It exited 0 on save failure and did nothing, silently, if the config failed to load. |
| gateway `config.set` / `config.apply` (`raw`) | `hu_config_save` | Same erasure, from the control protocol. |
| dashboard security toggles | `config.set {key, value}` | The handler read only `params.raw`, answered `saved:false`, and the dashboard never read `saved`: it toasted "Autonomy level set to N" for a write that never happened. |

Measured on real binaries with the same config (`initiative`,
`reaction_collection`, `router`, `voice`, `security`, `default_temperature`):
before, `workspace set` kept 2 of 6; after, 6 of 6 with `threshold` still
`0.85`.

The deleted serializer also wrote the sandbox backend as `"landlock_seccomp"`
while the parser only accepts `"landlock+seccomp"`, so a saved combined sandbox
came back as `auto` on the next load.

## Decisions

1. **One writer.** `config_mutator` edits the parsed file and re-renders it, so
   untouched keys survive. `hu_config_save` and its serializer are deleted, not
   kept: nothing called them after the reroute, and a lossy writer that exists
   gets reused.
2. **Validate the patch, not the file.** A mutation is refused, with nothing
   written, unless `{"a":{"b":value}}` passes the document half of the strict
   validator: known keys, correct types. Validating the whole file would let an
   unrelated key already in the user's config block every edit. Whole-config
   checks (provider, memory backend) are not applied. In the minimal build they
   judged the scratch config's *default* memory backend, compiled out there,
   and refused every write. `hu_config_validate_document` is the new name for
   that half; `hu_config_validate_strict` calls it unchanged.
3. **An unparseable existing file is refused, not replaced.** The old path
   treated it as `{}` plus one key, which erases everything.
4. **`gateway.require_pairing` stays off the allowlist.** An authenticated
   control-protocol session must not be able to switch off its own
   authentication. The dashboard now shows the refusal instead of a success
   toast. `config.*` methods already require an authenticated connection
   (`control_protocol.c`), so enabling the `security.*` keys the dashboard
   offers adds no new path for an unauthenticated caller.
5. **No default-path fallback in the gateway.** A config that was not loaded
   from a file has nothing to write. Falling back to the default path would
   have made `test_gateway_auth` overwrite the developer's real
   `~/.human/config.json` with `{}` on every test run.
6. **Floats render at the shortest precision that round-trips.** `%.17g` turned
   `0.85` into `0.84999999999999998`, so every edit would have rewritten every
   float the user wrote. `hu_json_stringify` now tries 15, 16 and 17 digits and
   keeps the first that parses back to the same double.
7. **Live config follows the file.** After a successful write the gateway
   applies the same patch to `app->config` and requests a reload, so the
   daemon's agent re-reads the file.

## Not done here

- `firecracker.c` writes a Firecracker VM config, not `config.json`; out of scope.
- Whole-config validation of `raw` documents (provider validity etc.) needs a
  validator that judges only what the document sets. Not built; the document
  check and the parse still apply.
