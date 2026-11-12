// swift-tools-version: 5.9
// Flutter (Swift Package Manager integration) links this package into Runner. The binary target is the engine built by
// scripts/ios/build-xcframework.sh (device + simulator slices, a dynamic framework Xcode embeds and signs). The Swift target is the
// plugin: engine start-up (GPU probe) and PhotoKit (dapple/media, dapple/photos channels).
import PackageDescription

let package = Package(
    name: "dapple_engine",
    platforms: [.iOS("16.0")],
    products: [
        .library(name: "dapple-engine", targets: ["dapple_engine"])
    ],
    dependencies: [
        .package(name: "FlutterFramework", path: "../FlutterFramework")
    ],
    targets: [
        .target(name: "dapple_engine",
                dependencies: ["dapple", .product(name: "FlutterFramework", package: "FlutterFramework")]),
        .binaryTarget(name: "dapple", path: "Frameworks/dapple.xcframework"),
    ]
)
