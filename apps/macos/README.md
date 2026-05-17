# macOS app

`Human.xcodeproj` is generated on demand from `project.yml` by XcodeGen and is
git-ignored. Install XcodeGen once (`brew install xcodegen`), then run
`xcodegen generate` in this directory before any `xcodebuild` invocation
(archive, build, test, or open-in-Xcode). Swift Package builds (`swift build`)
do not need the generated project.
