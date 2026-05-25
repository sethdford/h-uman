# Design for US-C1.4: Homebrew formula

**User Story:** As a user, I want to install human via Homebrew, so that `brew install human` works as an alternative to downloading the .pkg.

**Acceptance Criteria:**
- AC-C1.4.1: Formula file `Formula/human.rb` is valid Ruby and passes `brew audit --strict`; uses stable release (committed artifact or GitHub release tarball URL)
- AC-C1.4.2: Formula installs binaries to `$(brew --prefix)/bin/human` and `$(brew --prefix)/libexec/human_daemon`; launchd plist goes to `~/Library/LaunchAgents/com.human.daemon.plist`
- AC-C1.4.3: Formula includes a post-install step that enables the launchd plist via `launchctl load ~/Library/LaunchAgents/com.human.daemon.plist`; verifies daemon starts with `human --version` returning a version string
- AC-C1.4.4: `brew test human` runs a smoke test (daemon starts, version is readable); test must be able to run without network
- AC-C1.4.5: Test: `test_homebrew_formula.c` (or a shell script test) parses the formula, checks it references valid binaries from the release tarball, and verifies the launchd plist is valid XML with correct paths

---

## Approach

The Homebrew formula installs h-uman in two modes:
1. **Binary mode (default):** Downloads a pre-built macOS binary from GitHub Releases and installs to `$(brew --prefix)/bin/human`.
2. **Source mode (HEAD):** Clones from main and builds via `cmake --preset release`, installing the compiled binary.

The formula is already 85% complete (`Formula/human.rb` exists). This story finalizes it:
- **Validate the existing formula** against `brew audit --strict` and fix any issues.
- **Ensure launchd plist setup** is wired correctly: the formula installs a per-user plist to `~/Library/LaunchAgents/com.human.daemon.plist` and runs `launchctl load` post-install.
- **Test the formula** in CI using `brew install --build-from-source Formula/human.rb` on macOS runners.
- **Document the tap strategy**: Users tap `humanlabs/human` and run `brew install human`; this repo IS the tap for now.
- **Defer bottling** until signing/notarization ships (US-C1.3); unsigned bottles reduce tamper attribution clarity.

---

## Files to modify

| File | Change | Est. LOC |
|---|---|---|
| `Formula/human.rb` | Finalize launchd plist install, post_install hook, caveats; validate sha256 hashes for releases | +40 |
| `scripts/install/human-daemon.plist.template` | New launchd template with `${HOME}` and `${BREW_PREFIX}` substitutions; used by post_install | +30 |
| `tests/test_homebrew_formula.sh` | Bash test: validate formula syntax, plist XML structure, mock GitHub release tarball | +80 |
| `.github/workflows/test-homebrew-formula.yml` | New CI workflow: runs `brew audit`, `brew install --build-from-source`, verifies daemon starts | +40 |

**Total new:** ~190 LoC. No changes to C source or core build system.

---

## Implementation steps (for implementer)

1. **Create launchd plist template** (`scripts/install/human-daemon.plist.template`):
   - Copy and adapt `scripts/com.human.service.plist`
   - Replace hardcoded paths with `${HOME}`, `${BREW_PREFIX}` variables
   - Ensure `Program` points to `$(brew --prefix)/bin/human`
   - Ensure `ProgramArguments` includes `service-loop --with-gateway`

2. **Finalize `Formula/human.rb`**:
   - Verify sha256 hashes for current release binaries (v0.5.0 or current version)
   - Add `post_install` block that:
     - Renders the launchd template with actual home dir + brew prefix
     - Installs to `~/Library/LaunchAgents/com.human.daemon.plist`
     - Runs `launchctl load ~/Library/LaunchAgents/com.human.daemon.plist`
   - Add `caveats` block explaining Full Disk Access requirement, how to verify daemon is running (`launchctl list | grep com.human`)
   - Ensure `test` block runs `human --version` without spawning the daemon

3. **Create bash test script** (`tests/test_homebrew_formula.sh`):
   - Validate `Formula/human.rb` syntax with `ruby -c`
   - Validate against `brew audit --strict`
   - Check launchd plist XML structure (valid plist, contains expected keys)
   - Mock a release tarball and verify formula can extract the binary

4. **Create CI workflow** (`.github/workflows/test-homebrew-formula.yml`):
   - Trigger: on pull requests, pushes to main (can be manual for now)
   - Runs on `macos-latest` runner (or macOS 14 + 15 for coverage)
   - Step 1: Install Homebrew (usually pre-installed)
   - Step 2: Run `brew audit --strict Formula/human.rb`
   - Step 3: Run `bash tests/test_homebrew_formula.sh`
   - Step 4: Run `brew install --build-from-source Formula/human.rb` (compiles from HEAD)
   - Step 5: Verify `human --version` returns a version string
   - Step 6: Verify plist is installed and loadable (`launchctl list | grep com.human`)

---

## Risks

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| **sha256 mismatch for release binaries** | Medium | Large | Pre-flight: compute actual sha256 of v0.5.0 binaries from GitHub Releases before committing formula. Include in commit message. Re-validate before each release. |
| **Brew's Ruby DSL breaks on non-macOS CI** | Low | Medium | The formula has `on_macos do` guards; unsupported platforms gracefully skip. CI workflow runs on macOS runner only. |
| **CMakePresets.json unavailable in brew's sandboxed build** | Low | Medium | Brew clones the full repo including CMakePresets.json. If `--preset release` fails in the sandbox, fall back to explicit `-DCMAKE_BUILD_TYPE=MinSizeRel` flags (already in place). |
| **Launchd plist paths hardcoded, breaks on non-standard installs** | Low | Small | Use `${HOME}` and `$(brew --prefix)` substitutions in template; test renders correctly before install. |
| **User doesn't understand Full Disk Access requirement** | Medium | Medium | Caveats block explains requirement + links to Apple docs. `human doctor` can detect and suggest enabling. |
| **Notarization blocks binary installation** | Medium | Medium | Pre-built binaries are unsigned (notarization deferred to US-C1.3). Document this in caveats: "binary is unsigned; on macOS 13+, you may see a Gatekeeper warning. This is expected and safe." |

---

## Test strategy

### Unit tests
- Bash script (`tests/test_homebrew_formula.sh`): validates formula syntax, plist XML, audit pass/fail.
- No C tests needed (formula is Ruby/shell, not C code).

### Integration tests (CI workflow)
- Full build via `brew install --build-from-source Formula/human.rb` on macOS runner.
- Verify daemon binary exists at `$(brew --prefix)/bin/human`.
- Verify plist is installed and valid.
- Verify `human --version` returns non-empty string.

### AC mapping
- **AC-C1.4.1** → covered by `brew audit --strict` CI step
- **AC-C1.4.2** → covered by plist template + post_install block
- **AC-C1.4.3** → covered by post_install step that runs `launchctl load`
- **AC-C1.4.4** → covered by CI step that runs `human --version` and checks plist
- **AC-C1.4.5** → covered by bash test script that validates plist XML + formula structure

---

## Tap & Bottling Strategy

**Tap location (now):** This repo is the tap. Users run:
```bash
brew tap humanlabs/human https://github.com/sethdford/h-uman.git
brew install human
```

The tap is rooted at `Formula/human.rb`; Homebrew will find it automatically.

**Bottling (deferred to US-C1.3 or later):**
- Current formula downloads pre-built *binaries*, not bottles.
- A bottle is a pre-compiled `*.tar.gz` with relocation metadata, tested by Homebrew's CI.
- Bottling requires signed/notarized binaries (US-C1.3 work). Unsigned bottles reduce tamper attribution.
- **Recommendation:** No bottles until first stable release + signing lands. The formula will build-from-source until then.

**Future (not in scope):**
- Homebrew-core submission requires stable release + 100+ community stars. Not yet applicable.

---

## Out of scope

- **Auto-update mechanism** (C5 scope): Homebrew handles updates via `brew upgrade`. No in-app updater.
- **Multi-version management** (brew pinning): Users can install specific versions via `@` syntax (standard brew feature).
- **Windows/Linux Homebrew variants**: Formula is macOS-focused (already has `on_macos` guards); cross-platform packaging is separate.
- **Sandbox entitlements**: Signed binaries come later (US-C1.3).
- **GUI installer** (macOS installer.app): Out of scope; this is CLI-focused.

---

## Acceptance Criteria Verification Checklist

- [ ] `Formula/human.rb` is syntactically valid Ruby: `ruby -c Formula/human.rb` passes
- [ ] `brew audit --strict Formula/human.rb` passes
- [ ] Launchd plist template is valid XML with correct daemon invocation
- [ ] `brew install --build-from-source Formula/human.rb` succeeds on macOS CI
- [ ] `human --version` returns a version string post-install
- [ ] Plist is installed to `~/Library/LaunchAgents/com.human.daemon.plist`
- [ ] `launchctl list | grep com.human` shows the daemon (or indicates it's not running due to Full Disk Access)
- [ ] All tests in `tests/test_homebrew_formula.sh` pass
- [ ] CI workflow `.github/workflows/test-homebrew-formula.yml` runs successfully on macOS runners
- [ ] No secrets leaked in workflow logs
- [ ] Caveats block explains Full Disk Access requirement
- [ ] Formula passes full test suite (no regressions)

