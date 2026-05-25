# Human.app Bundle Layout

This directory (`apps/macOS/Human.app/`) is the source template for the macOS application bundle skeleton. It defines the directory structure and metadata required for a proper macOS .app application.

## Directory Structure

```
Human.app/
├── Contents/
│   ├── Info.plist          # Bundle metadata (name, version, executable, bundle ID)
│   ├── MacOS/              # Executable binaries
│   │   └── human           # Daemon executable (copied by CMake build)
│   ├── Resources/          # Localized strings, images, assets (future)
│   └── _CodeSignature/     # Code signature artifacts (created by codesign tool)
└── BUNDLE_LAYOUT.md        # This file
```

## Build Process

The CMake build system wires the bundle assembly via the `build_app_bundle` target (in `CMakeLists.txt`):

1. Creates the directory structure under `${CMAKE_BINARY_DIR}/Human.app/Contents/`
2. Copies the compiled `build/human` daemon binary to `Contents/MacOS/human`
3. Sets executable permissions (755) on the binary
4. Copies `Info.plist` from this directory to `Contents/Info.plist`
5. Verifies no `@rpath` entries in the binary (ensures relocatability)

## Info.plist Keys

The Info.plist must contain these keys per macOS application specification:

- **CFBundleIdentifier**: `ai.human.macos` — unique identifier for the app
- **CFBundleName**: `Human` — display name
- **CFBundlePackageType**: `APPL` — identifies this as an application
- **CFBundleVersion**: Semantic version (e.g., `0.5.0`)
- **CFBundleExecutable**: `human` — name of the executable in `Contents/MacOS/`
- **LSMinimumSystemVersion**: `14.0` — minimum macOS version (Sonoma)
- **CFBundleDevelopmentRegion**: `en` — default language
- **NSAppleEventsUsageDescription**: Explanation for AppleEvent permissions

## Contents/MacOS

This directory holds the executable binaries. Currently only the daemon (`human`) is placed here. The CLI binary may be added in future stories if needed.

At build time, CMake copies the compiled daemon binary here and ensures it is executable.

## Contents/Resources

Reserved for future use:
- Localized strings (`.strings` files)
- Application icons (`Icon.icns`)
- Other asset files

Currently empty (marked with `.gitkeep` for version control).

## Contents/_CodeSignature

Created by the `codesign` tool during the signing process (US-C1.3). Contains signature metadata. Not committed to source control.

## Verification

To verify the bundle structure is valid:

```bash
./scripts/release/verify-bundle.sh [--app-path <path>]
```

This script checks:
- Directory structure exists
- Info.plist is valid XML
- Required keys are present
- Binary is executable
- No @rpath entries in the binary

## Related Stories

- **US-C1.1** (this story): Bundle skeleton and CMake wiring
- **US-C1.2**: Converts the bundle into a `.pkg` installer
- **US-C1.3**: Adds code signing and notarization
- **US-C1.4**: Homebrew formula for alternative installation

## Testing

The test suite verifies bundle structure in `tests/test_app_bundle_structure.c`:
- Checks directory tree exists
- Validates Info.plist XML syntax
- Confirms required keys are present
- Verifies daemon binary is present and executable

Run with: `./build/human_tests --suite=app_bundle_structure`
