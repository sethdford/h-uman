# Design for US-C1.2: macOS .pkg Installer Build Script

## Approach

US-C1.2 bridges the gap between the bundled Human.app (from US-C1.1) and a distributable .pkg installer. The design follows macOS native packaging: `pkgbuild` creates a component package with correct ownership/permissions, and `productbuild` wraps it with a distribution policy (system version check, free space validation) and UI metadata. The script emits a single unsigned .pkg ready for code-signing in US-C1.3.

Why this design and not alternatives:
- **pkgbuild + productbuild** is the macOS native toolchain (Apple's recommended path; no third-party dependency).
- **Distribution.xml** enables system requirements (macOS 10.15+ check, 500 MB free space) without installer UI code.
- **Flat .pkg** (not a bundle) is what Gatekeeper expects; productbuild produces it directly.
- **Pre/post scripts** are minimized: pre-install is empty, post-install loads launchd plist only (no interactive permission prompts; those happen on first daemon run via UI, not via installer).
- **Launchd plist as shipped asset** (not templated during install) keeps the component deterministic and signable as a unit.

## Files to modify

| File | Change | Est. LOC |
|---|---|---|
| `scripts/release/build-pkg.sh` | New script: cmake + strip + pkgbuild + productbuild invocation | +180 |
| `scripts/release/com.h-uman.human.plist` | New launchd plist: daemon auto-start config | +35 |
| `CMakeLists.txt` | Add conditional test for `pkgbuild` availability (exit 79 on non-macOS) | +12 |
| `tests/test_pkg_builder.c` | New unit tests: mock pkgbuild invocation, verify command-line flags | +100 |
| `tests/fixtures/distribution.xml.template` | Distribution policy XML for productbuild | +25 |

**Total new LOC: ~352**

## Implementation steps (for the implementer)

1. **Verify US-C1.1 inputs**: `build/Release/Human.app` exists with Info.plist (CFBundleVersion, CFBundleIdentifier = "ai.human.daemon").

2. **Create launchd plist** at `scripts/release/com.h-uman.human.plist`:
   - Label: `com.h-uman.human` (reverse-domain from app bundle ID)
   - ProgramArguments: `["/Applications/Human.app/Contents/MacOS/human", "service-loop", "--with-gateway"]`
   - RunAtLoad: true, KeepAlive: true (daemon respawns if killed)
   - StandardOutPath/StandardErrorPath: `~/.human/human.log`
   - ThrottleInterval: 10 (wait 10 sec before respawning after crash)
   - ProcessType: Background

3. **Create Distribution.xml template** at `tests/fixtures/distribution.xml.template`:
   - Title: "human"
   - Version: extracted from CMakeLists.txt or Info.plist at script runtime
   - Options: allow-external-scripts (for post-install hook)
   - InstallationCheck script: macOS version >= 10.15 (Catalina) + 500 MB free
   - VolumeCheck: requires at least 500 MB free

4. **Create build-pkg.sh**:
   - Signature: `build-pkg.sh [--version <semver>] [--app-path <path>] [--output <path>]`
   - Defaults: version from CMakeLists.txt, app-path=`build/Release/Human.app`, output=`./human-release.pkg`
   - Steps:
     a. Validate app-path exists and is executable
     b. Extract version from app-path's Info.plist (CFBundleVersion key)
     c. Create staging dir: `tmp.pkg-staging-$$` with Contents/{MacOS,Resources}
     d. `ditto` copy Human.app into staging (preserves permissions)
     e. Strip binaries in staging (optional but recommended for size)
     f. Create component.plist (defines install location as /Applications/Human.app)
     g. `pkgbuild --root staging --component-plist component.plist --identifier com.h-uman.human.pkg --version <version> component.pkg`
     h. Substitute version into Distribution.xml template
     i. `productbuild --distribution distribution.xml --resources resources/ --package-path . output.pkg`
     j. Verify output.pkg is >= 8 MB and valid (pkgutil --check-signature)
     k. Clean up temp files

5. **Add CMake test gate**:
   - Check for `pkgbuild` binary in PATH
   - If not found and running on non-macOS CI, skip test with exit 79 (HU_SKIP_PLATFORM_SPECIFIC)
   - If found, compile and run `test_pkg_builder.c`

6. **Write test_pkg_builder.c**:
   - Mock `hu_shell_exec` to capture pkgbuild invocation
   - Test AC-C1.2.1: script accepts flags and parses them correctly
   - Test AC-C1.2.2: pkgbuild command includes correct paths, ownership, perms
   - Test AC-C1.2.3: Distribution.xml is generated with version substituted
   - Test AC-C1.2.4: output file size >= 8 MB (integration test only, marked HU_IS_TEST to skip on non-macOS)
   - Test AC-C1.2.5: mock pkgbuild and verify exit code 0

## Risks

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| **pkgbuild not available on non-macOS CI** | High | Medium | Test gated by CMake check; returns exit 79 (unsupported platform) if pkgbuild missing. CI runner skips the test. |
| **Human.app missing executable bit** | Medium | Medium | Script validates `test -x app-path/Contents/MacOS/human` and exits 1 with clear message. Test pinned by checking executable-bit preservation in mock. |
| **Info.plist missing CFBundleVersion key** | Low | Medium | Script uses `plutil -p` or `grep` to extract version; fails clearly if key missing. Fallback to `git describe --tags` if Info.plist doesn't have version. |
| **Distribution.xml syntax error** | Low | Small | Syntax validated by `productbuild --validate-only distribution.xml` before final build. Test includes valid fixture. |
| **pkgbuild version quirks (Xcode 14 vs 15)** | Low | Small | Script emits all verbose flags; test mocks invocation. Actual invocation is tested on macOS CI only. |
| **Permissions lose fidelity in staging** | Low | Medium | Use `ditto` (preserves ACLs + xattrs) instead of `cp -r`. Test verifies owner=root:wheel, mode=755 on /Applications/Human.app in mocked pkgbuild call. |

## Test strategy

**Unit tests** (`tests/test_pkg_builder.c`):
- Mock `hu_shell_exec` to capture pkgbuild/productbuild invocations
- 5–8 test cases covering flag parsing, command generation, Distribution.xml templating
- Run on all platforms (mocks skip actual pkgbuild)

**Integration test** (runs only on macOS CI with HU_IS_TEST gating):
- Actually invoke build-pkg.sh with a test app bundle
- Verify output.pkg file exists, is >= 8 MB, and `file` returns "xar archive"
- Verify `pkgutil --check-signature output.pkg` succeeds (unsigned .pkg is still valid; signature check passes on empty cert)
- Verify `pkgutil --payload-files output.pkg | grep -q 'Applications/Human.app'` (payload contains the bundle)

**No mocking in integration test** — that's the whole point; we confirm real pkgbuild succeeds.

## Acceptance criteria mapping

- **AC-C1.2.1** → `test_pkg_builder.c::test_flag_parsing` (verifies --output, --app-path, defaults)
- **AC-C1.2.2** → `test_pkg_builder.c::test_pkgbuild_command_ownership` (mocks pkgbuild, asserts command includes `--ownership=preserve` and correct paths)
- **AC-C1.2.3** → `test_pkg_builder.c::test_distribution_xml_version_substitution` (verifies template has InstallationCheck + macOS version + free-space requirement)
- **AC-C1.2.4** → Integration test on macOS CI: `file ./human-release.pkg` + size check
- **AC-C1.2.5** → `test_pkg_builder.c::test_mock_pkgbuild_success` (mocks tool, verifies clean exit)

## Out of scope

- **Code signing / notarization** → US-C1.3
- **Homebrew formula** → US-C1.4
- **CI workflow wiring** → US-C1.5
- **GUI installer / wizard** → C2 scope (onboarding UX)
- **Installer UI customization** (brand, localization) → P1/P2 (Design phase)
- **Multi-user / enterprise scenarios** → Non-goal for Sprint C

## Launchd Plist Design Detail

The plist at `scripts/release/com.h-uman.human.plist` is a **shipped asset** (not templated during install). Its path is NOT `/Library/LaunchAgents/` or `/Library/LaunchDaemons/` at install time; instead, the post-install script copies it into the user's LaunchAgents directory:

```bash
# In post-install script (if added in later story):
cp "$PAYLOAD_DIR/com.h-uman.human.plist" \
   ~/.human/com.h-uman.human.plist
launchctl load ~/.human/com.h-uman.human.plist
```

**Why**: The .pkg is designed for /Applications install (system-wide, read-only). LaunchAgents are per-user (~/.../LaunchAgents). The post-install script runs as the user, so it can place the plist in the right location and load it in that user's session only.

For Sprint C, the post-install step is a stub ("Install successful"). LaunchAgent loading is left to the onboarding wizard (US-C2.2).

## Failure Modes & Diagnostics

| Failure | Signal | Mitigation |
|---|---|---|
| `pkgbuild: command not found` | Script exits 1 immediately | CI only runs on macOS runners; non-macOS CI has pkgbuild check gate that skips test |
| `Human.app not executable` | Script emits "error: Contents/MacOS/human is not executable" | AC-C1.2.2 covers this; test pinned |
| `Distribution.xml invalid XML` | `productbuild` returns exit code 1 | Script validates with `productbuild --validate-only` before final build; test includes valid fixture |
| `.pkg file < 8 MB` | Script emits warning but continues (size depends on binary optimization; 8 MB is a heuristic lower bound) | Test mocks and skips size check for determinism; integration test on CI validates actual size |
| `Free space check fails in InstallationCheck script` | User sees installer error dialog; install aborts | Tested in Distribution.xml fixture; real validation happens only on user's machine during install |

## Inputs & Outputs

**Inputs (from US-C1.1)**:
- `build/Release/Human.app/Contents/Info.plist` — contains CFBundleVersion + CFBundleIdentifier
- `build/Release/Human.app/Contents/MacOS/{human, human_cli}` — compiled binaries
- `build/Release/Human.app/Contents/Resources/**` — resource files (if any)

**Outputs (for US-C1.3)**:
- `./human-release.pkg` (or `--output <path>`) — unsigned, flat .pkg file
- `build/Release/Human.app` remains untouched (signed separately in US-C1.3)

## Build Pipeline Pseudocode

```bash
# High-level flow of build-pkg.sh
STAGE=$(mktemp -d)
ditto "$APP_PATH" "$STAGE/Applications/Human.app"

# Verify executable bit
test -x "$STAGE/Applications/Human.app/Contents/MacOS/human" || exit 1

# Extract version
VERSION=$(plutil -p "$STAGE/Applications/Human.app/Contents/Info.plist" | grep CFBundleVersion | cut -d'"' -f4)

# Create component.plist (defines install root)
cat > component.plist <<EOF
<dict>
  <key>BundleIsRelocatable</key><true/>
  <key>BundleIsVersionChecked</key><false/>
  <key>BundleOverwriteAction</key><string>update</string>
  <key>RootRelativeBundlePath</key><string>Human.app</string>
</dict>
EOF

# Build component package
pkgbuild --root "$STAGE" \
         --component-plist component.plist \
         --identifier "com.h-uman.human.pkg" \
         --version "$VERSION" \
         component.pkg

# Build distribution package (wraps component with policy)
productbuild --distribution distribution.xml \
             --package-path . \
             "$OUTPUT"

# Verify output
pkgutil --check-signature "$OUTPUT" || exit 1
ls -lh "$OUTPUT"  # Should be >= 8 MB

rm -rf "$STAGE" component.plist
```

## Related

- `CLAUDE.md` — C1 mission ("Ship to Users: 100 DAU")
- `sprints/sprint-49/designs/US-C1.1.md` — app bundle structure (input)
- `sprints/sprint-49/designs/US-C1.3.md` — code signing (consumer of this .pkg)
- `docs/standards/engineering/` — build and release standards
- `scripts/release.sh` — existing release script (version bumping, tagging)
