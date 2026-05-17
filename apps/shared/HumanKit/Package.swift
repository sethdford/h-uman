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
        .library(name: "HumanOnDevice", targets: ["HumanOnDevice"]),
        .library(name: "HumanOnDeviceServer", targets: ["HumanOnDeviceServer"]),
    ],
    targets: [
        .target(
            name: "HumanProtocol",
            path: "Sources/HumanProtocol",
            swiftSettings: [.unsafeFlags(["-strict-concurrency=complete"])]
        ),
        .target(
            name: "HumanClient",
            dependencies: ["HumanProtocol"],
            path: "Sources/HumanClient",
            swiftSettings: [.unsafeFlags(["-strict-concurrency=complete"])]
        ),
        .target(
            name: "HumanChatUI",
            dependencies: ["HumanProtocol"],
            path: "Sources/HumanChatUI",
            swiftSettings: [.unsafeFlags(["-strict-concurrency=complete"])]
        ),
        .target(
            name: "HumanOnDevice",
            path: "Sources/HumanOnDevice"
        ),
        .target(
            name: "HumanOnDeviceServer",
            dependencies: ["HumanOnDevice"],
            path: "Sources/HumanOnDeviceServer"
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
        .testTarget(
            name: "HumanChatUITests",
            dependencies: ["HumanChatUI"],
            path: "Tests/HumanChatUITests"
        ),
        .testTarget(
            name: "HumanOnDeviceTests",
            dependencies: ["HumanOnDevice"],
            path: "Tests/HumanOnDeviceTests"
        ),
        .testTarget(
            name: "HumanOnDeviceServerTests",
            dependencies: ["HumanOnDeviceServer"],
            path: "Tests/HumanOnDeviceServerTests"
        ),
    ]
)
