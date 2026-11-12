// swift-tools-version: 5.9
// Flutter (Swift Package Manager integration) links this package into Runner. The binary target is the engine built by
// scripts/ios/build-xcframework.sh (device + simulator slices, a dynamic framework Xcode embeds and signs).
import PackageDescription

let package = Package(
    name: "dapple_engine",
    platforms: [.iOS("16.0")],
    products: [
        .library(name: "dapple-engine", targets: ["dapple_engine"])
    ],
    targets: [
        .target(name: "dapple_engine", dependencies: ["dapple"]),
        .binaryTarget(name: "dapple", path: "Frameworks/dapple.xcframework"),
    ]
)
