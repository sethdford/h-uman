// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "HumaniOS",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [
        .library(name: "HumaniOS", targets: ["HumaniOS"]),
    ],
    dependencies: [
        .package(path: "../shared/HumanKit"),
    ],
    targets: [
        .target(
            name: "HumaniOS",
            dependencies: [
                .product(name: "HumanClient", package: "HumanKit"),
                .product(name: "HumanChatUI", package: "HumanKit"),
                .product(name: "HumanProtocol", package: "HumanKit"),
            ],
            path: "Sources/HumaniOS"
        ),
    ]
)
