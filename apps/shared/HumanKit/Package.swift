// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "HumanKit",
    platforms: [
        .macOS(.v14),
        .iOS(.v17)
    ],
    products: [
        .library(name: "HumanProtocol", targets: ["HumanProtocol"]),
        .library(name: "HumanClient", targets: ["HumanClient"]),
        .library(name: "HumanChatUI", targets: ["HumanChatUI"]),
    ],
    targets: [
        .target(
            name: "HumanProtocol",
            path: "Sources/HumanProtocol"
        ),
        .target(
            name: "HumanClient",
            dependencies: ["HumanProtocol"],
            path: "Sources/HumanClient"
        ),
        .target(
            name: "HumanChatUI",
            dependencies: ["HumanProtocol"],
            path: "Sources/HumanChatUI"
        ),
        .testTarget(
            name: "HumanProtocolTests",
            dependencies: ["HumanProtocol"],
            path: "Tests/HumanProtocolTests"
        ),
        .testTarget(
            name: "HumanClientTests",
            dependencies: ["HumanClient"],
            path: "Tests/HumanClientTests"
        ),
    ]
)
