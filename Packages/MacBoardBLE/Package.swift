// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "MacBoardBLE",
    platforms: [
        .macOS(.v14),
    ],
    products: [
        .library(name: "MacBoardBLE", targets: ["MacBoardBLE"]),
    ],
    targets: [
        .target(name: "MacBoardBLE"),
    ]
)
