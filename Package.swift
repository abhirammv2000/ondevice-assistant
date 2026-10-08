// swift-tools-version:6.0
import PackageDescription

// The C++ core and its C interface are compiled straight into the package, so there is nothing to install first.
// CAssist is the C target the Swift code imports; AssistKit is the Swift API on top of it.
let package = Package(
    name: "AssistKit",
    platforms: [.iOS(.v16), .macOS(.v13)],
    products: [
        .library(name: "AssistKit", targets: ["AssistKit"]),
    ],
    targets: [
        .target(
            name: "CAssist",
            path: ".",
            sources: ["core/src", "capi/capi.cpp"],
            publicHeadersPath: "capi/include",
            cxxSettings: [.headerSearchPath("core/include")]
        ),
        .target(
            name: "AssistKit",
            dependencies: ["CAssist"],
            path: "swift/Sources/AssistKit"
        ),
        .testTarget(
            name: "AssistKitTests",
            dependencies: ["AssistKit"],
            path: "swift/Tests/AssistKitTests"
        ),
    ],
    cxxLanguageStandard: .cxx20
)
