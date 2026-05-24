// swift-tools-version: 6.0
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
            swiftSettings: [.swiftLanguageMode(.v6)]
        ),
        .target(
            name: "HumanClient",
            dependencies: ["HumanProtocol"],
            path: "Sources/HumanClient",
            swiftSettings: [.swiftLanguageMode(.v6)]
        ),
        .target(
            name: "HumanChatUI",
            dependencies: ["HumanProtocol"],
            path: "Sources/HumanChatUI",
            swiftSettings: [.swiftLanguageMode(.v6)]
        ),
        .target(
            name: "HumanOnDevice",
            path: "Sources/HumanOnDevice",
            swiftSettings: [.swiftLanguageMode(.v6)]
        ),
        .target(
            name: "HumanOnDeviceServer",
            dependencies: ["HumanOnDevice"],
            path: "Sources/HumanOnDeviceServer",
            swiftSettings: [.swiftLanguageMode(.v6)]
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
