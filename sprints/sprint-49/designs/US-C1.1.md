# Design for US-C1.1: macOS App Bundle Skeleton

## Approach

Create a minimal macOS `.app` bundle structure under `apps/macOS/Human.app/Contents/` that follows Apple's standard bundle layout. The bundle contains the daemon binary (`human`) and optional CLI executable in `MacOS/`, an `Info.plist` with required keys for macOS, and a stub `_CodeSignature/` directory placeholder. Wire CMakeLists.txt with a new `build_app_bundle` target that copies the compiled binaries into the bundle and verifies readiness for signing (no rpath, system libc only). The design prioritizes simplicity: no code-signing, no entitlements file yet (deferred to US-C1.3), no launchd plist (that's C2/C3 onboarding scope). The bundle is a **passive container** — just layout and metadata — so the daemon runs identically whether invoked as `build/human` or `Human.app/Contents/MacOS/human`.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `apps/macOS/Human.app/Contents/Info.plist` | Create; macOS 14.0+, bundle ID, version, executable name | +40 |
| `apps/macOS/Human.app/Contents/MacOS/.gitkeep` | Directory placeholder for daemon and CLI binaries | +0 |
| `apps/macOS/Human.app/Contents/Resources/.gitkeep` | Reserved for future localization / assets | +0 |
| `CMakeLists.txt` | Add `build_app_bundle` target; copy binaries; verify dependencies | +120 |
| `tests/test_app_bundle_structure.c` | Verify bundle layout, Info.plist validity, executable presence | +140 |

## Implementation steps (for the implementer)

1. **Create bundle directory structure:**
   - `mkdir -p apps/macOS/Human.app/Contents/{MacOS,Resources,_CodeSignature}`
   - Touch `.gitkeep` in `MacOS` and `Resources` to version-control the empty directories

2. **Write Info.plist at `apps/macOS/Human.app/Contents/Info.plist`:**
   - Required keys: `CFBundleIdentifier` (ai.human.macos), `CFBundleName`, `CFBundlePackageType` (APPL), `CFBundleVersion` (from git tag or semver), `CFBundleExecutable` (human)
   - Optional but important: `CFBundleDevelopmentRegion`, `LSMinimumSystemVersion` (14.0), `LSUIElement` (false — this is a daemon, but can accept user interaction via terminal), `NSAppleEventsUsageDescription` (existing key from resources/Info.plist)
   - Version strategy: read `git describe --tags --abbrev=0` at CMake time; fall back to `0.0.0-dev` if no tags exist

3. **Add CMakeLists.txt target `build_app_bundle`:**
   - Depends on `human` executable target
   - Copy `${CMAKE_BINARY_DIR}/human` → `${CMAKE_BINARY_DIR}/Human.app/Contents/MacOS/human`
   - Verify `otool -L Human.app/Contents/MacOS/human` returns only system frameworks (e.g., libSystem, no rpath entries)
   - Set executable bit (chmod 755) on the binary
   - Create a symbolic link or copy `Info.plist` from `apps/macOS/Human.app/Contents/` into the built bundle
   - Make this target a dependency of the default build (optional: add to ALL, or require explicit invocation)

4. **Write test file `tests/test_app_bundle_structure.c`:**
   - Happy path: bundle directory exists, Info.plist is valid XML (use plist parsing if available, else accept `plutil -lint`)
   - Verify `MacOS/human` exists and has execute bit set
   - Verify `Info.plist` contains required keys (CFBundleIdentifier, CFBundleVersion, CFBundleExecutable, CFBundlePackageType)
   - Verify `CFBundlePackageType` value is `APPL`
   - Edge case: handle missing bundle (test should fail gracefully; build system responsibility)
   - Do NOT attempt to run the binary; that's integration test scope

5. **Run full test suite to confirm no regressions:**
   - `./build/human_tests` must still pass
   - New test suite must pass

## Risks

- **Xcode version drift (LOW/SMALL):** Info.plist keys are stable across macOS versions. Mitigation: document expected keys in a comment in the plist; test validates presence of critical keys so future maintainers see clear errors if a key is lost.
- **Binary rpath leakage (MEDIUM/MEDIUM):** CMake might set rpath to build/ without care, breaking the bundle on deploy. Mitigation: `otool -L` verification step in build target fails loudly if rpath is detected; CI will catch this before merge.
- **macOS SDK mismatch (LOW/SMALL):** Different SDK versions may add/remove keys. Mitigation: hardcode only keys stable since macOS 13.0; LSMinimumSystemVersion gates to 14.0. Future enhancements (e.g., privacy entitlements) go in US-C1.3 with explicit documentation.
- **Test false positives (LOW/SMALL):** Test may pass on dev machine but fail in CI if bundle layout differs. Mitigation: test uses absolute paths (CMake-generated) not relative paths; CI uses same CMake build flow.
- **Observability (LOW):** Hard to debug bundle issues without inspection tools. Mitigation: build target prints diagnostic info (paths, file listing, otool output) to stdout when HU_VERBOSE is set.

## Test strategy

- **Unit test:** `tests/test_app_bundle_structure.c` verifies static properties (file exists, XML valid, keys present). Runs on all platforms (test checks if bundle exists; skips gracefully if not on macOS).
- **Integration test:** CI runs `plutil -lint` + `codesign --verify --deep` on the built bundle (no signing cert needed; codesign just verifies structure). This catches layout issues early.
- **Manual smoke test (post-merge):** `otool -L build/Human.app/Contents/MacOS/human | grep -c "@rpath"` must return 0.

## Acceptance criteria mapping

- **AC-C1.1.1** → Directory tree + Info.plist with required keys: satisfied by step 1–2, verified by test
- **AC-C1.1.2** → CMakeLists.txt build_app_bundle target: satisfied by step 3
- **AC-C1.1.3** → cmake build produces Human.app with binaries and readable perms: satisfied by step 3 (chmod 755), verified by test
- **AC-C1.1.4** → otool -L shows no rpath: satisfied by step 3 (otool verification), CI smoke test confirms
- **AC-C1.1.5** → test_app_bundle_structure.c passes: satisfied by step 4

## Out of scope

- **Code signing** — US-C1.3
- **Entitlements file** — US-C1.3 (macOS security permissions)
- **Launchd plist** — C2/C3 onboarding (daemon auto-start on login)
- **App icons, localization** — future sprints (not required for technical MVP)
- **CLI binary placement** — use case TBD; scaffold MacOS/ directory to accept it if future stories add it
- **Universal binary (arm64 + x86_64)** — Sprint C ships current host arch only; multi-arch is C5 scope

## Dependencies

**Upstream (blocking this story):**
- CMake 3.20+ (already required)
- macOS SDK (Xcode Command Line Tools)

**Downstream (blocked by this story):**
- US-C1.2 (build-pkg.sh needs Human.app bundle)
- US-C1.3 (code signing needs valid bundle)
- US-C1.4 (brew formula references Human.app)
