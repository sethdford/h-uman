# Human — macOS app

The macOS app is built two ways:

- **SwiftPM** (`Package.swift`) — the day-to-day dev loop. `swift build`,
  `swift run`, `swift test` work as usual.
- **Xcode project** (generated from `project.yml`) — required to produce
  a codesigned `.xcarchive` for distribution. CI uses this path; see the
  `macos-app-archive` job in `.github/workflows/native-apps-fleet.yml`.

## Why XcodeGen

SwiftPM cannot produce a codesigned `.xcarchive`. To keep the `.xcodeproj`
out of the repo (so it does not drift, get partially committed, or
conflict on merges), we generate it on demand from a small declarative
`project.yml`. Same pattern as `apps/ios/project.yml`.

## Prerequisites

```sh
brew install xcodegen
```

XcodeGen is currently consumed unpinned; the runner image's
homebrew-core revision is the floor. Pin to `xcodegen@<version>` in a
follow-up if reproducibility becomes an issue.

## Generating the Xcode project

From this directory:

```sh
cd apps/macos
xcodegen generate
open Human.xcodeproj
```

The generated `Human.xcodeproj/` is git-ignored (see `.gitignore`). It
must be regenerated locally before opening in Xcode after any change to
`project.yml` or the SwiftPM dependency graph.

## Archiving (codesigned)

CI is the contract. To reproduce locally with an Apple Development
identity already in your `login.keychain`:

```sh
cd apps/macos
xcodegen generate
xcodebuild archive \
    -project Human.xcodeproj \
    -scheme Human \
    -configuration Release \
    -archivePath build/Human.xcarchive \
    CODE_SIGN_IDENTITY="Apple Development" \
    CODE_SIGN_STYLE=Manual
codesign --verify --deep --strict \
    build/Human.xcarchive/Products/Applications/Human.app
```

`--deep` is passed only to `codesign --verify`, never to
`xcodebuild archive`. The archive invocation re-signs only the top-level
bundle; `--deep` on the verify step walks the bundle tree to confirm
every nested signature is intact.

## Out of scope (other US-45.x stories)

- Notarization, stapling, DMG packaging — US-45.2.
- Hardened-runtime entitlements — US-45.6.
- Sparkle auto-update — beyond Sprint 45.
