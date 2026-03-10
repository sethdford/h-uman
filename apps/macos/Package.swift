// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "Human",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "Human",
            path: "Sources/HumanApp"
        ),
    ]
)
