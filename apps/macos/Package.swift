// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "Human",
    platforms: [.macOS(.v14)],
    dependencies: [
        .package(path: "../shared/HumanKit"),
    ],
    targets: [
        .executableTarget(
            name: "Human",
            dependencies: [
                .product(name: "HumanClient", package: "HumanKit"),
                .product(name: "HumanChatUI", package: "HumanKit"),
                .product(name: "HumanOnDevice", package: "HumanKit"),
                .product(name: "HumanOnDeviceServer", package: "HumanKit"),
                .product(name: "HumanVoice", package: "HumanKit"),
            ],
            path: "Sources/HumanApp"
        ),
    ]
)
