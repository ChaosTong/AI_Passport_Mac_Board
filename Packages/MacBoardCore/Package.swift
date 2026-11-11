// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "MacBoardCore",
    platforms: [
        .macOS(.v14),
    ],
    products: [
        .library(name: "MacBoardCore", targets: ["MacBoardCore"]),
    ],
    dependencies: [
        .package(path: "../MacBoardBLE"),
    ],
    targets: [
        .target(
            name: "MacBoardCore",
            dependencies: [
                .product(name: "MacBoardBLE", package: "MacBoardBLE"),
            ]
        ),
    ]
)
