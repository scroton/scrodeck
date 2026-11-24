// swift-tools-version:5.9
// mac/Package.swift — Claude Deck, the macOS shell, on the shared C++ core.
// UNBUILT, UNTESTED: written on Windows with no Swift toolchain. mac/README.md "First-build checklist" is the
// order to bring it up in; every "VERIFY" below is a thing to check on the first build.
//
// Assumed toolchain: Xcode 15.3+ (Swift 5.10; tools 5.9 is the floor because that is where C++ interop and
// `cxxLanguageStandard` + mixed C-family targets are settled). macOS 13 is the deployment floor (Metal 3-era
// MTKView, UNUserNotificationCenter, NWPathMonitor are all older; 13 is simply what is still patched).
//
// Targets
//   DeckCore         C++17  the repo's core/ (Sources/DeckCore/core is a SYMLINK to ../../../core)
//   DeckCoreBridge   C++17  the C ABI over it (include/DeckBridge.h) — built and tested on Windows (Tests/build.ps1)
//   DeckMacSeams     C++ / Objective-C++  the macOS seams (forkpty, POSIX files, libproc, sockets, URLSession, Keychain)
//   ClaudeDeck       Swift  the AppKit + Metal app
import PackageDescription

// "core/deck/deck.hpp" and "windows/src/mouse.hpp" resolve from here in every C++ target
let coreHeaders: [CXXSetting] = [.headerSearchPath("../DeckCore")]

let package = Package(
  name: "ClaudeDeckMac",
  platforms: [.macOS(.v13)],
  products: [.executable(name: "ClaudeDeck", targets: ["ClaudeDeck"])],
  targets: [
    .target(
      name: "DeckCore",
      path: "Sources/DeckCore",
      // VERIFY: SwiftPM follows the `core` directory symlink out of the package root. If it refuses, the fallback
      // is in mac/README.md ("If SwiftPM will not follow the symlink").
      exclude: [
        "core/test", "core/CMakeLists.txt", "core/README.md", "core/PORTING.md", "core/PROTOCOL.md",
        "core/deck/README.md", "core/third_party/nlohmann/LICENSE.MIT",
        "core/vt/charsets.inc", "core/vt/unicode_v6.inc",      // #included by the .cpp beside them, not compiled
        "windows",                                              // one header (mouse.hpp), included by the bridge
      ],
      publicHeadersPath: "include",
      cxxSettings: [.define("NDEBUG", .when(configuration: .release))]
    ),
    .target(
      name: "DeckCoreBridge",
      dependencies: ["DeckCore"],
      path: "Sources/DeckCoreBridge",
      publicHeadersPath: "include",
      cxxSettings: coreHeaders
    ),
    .target(
      name: "DeckMacSeams",
      dependencies: ["DeckCoreBridge"],
      path: "Sources/DeckMacSeams",
      publicHeadersPath: "include",
      cxxSettings: coreHeaders,   // VERIFY: Objective-C++ (.mm) files take cxxSettings and are compiled with ARC (SwiftPM's default)
      linkerSettings: [.linkedFramework("Foundation"), .linkedFramework("Security"), .linkedFramework("CoreFoundation")]
    ),
    .executableTarget(
      name: "ClaudeDeck",
      dependencies: ["DeckCoreBridge", "DeckMacSeams"],
      path: "Sources/ClaudeDeck",
      // The boundary is a C ABI on purpose (mac/README.md "Why a C façade"), so the Swift target does NOT need
      // C++ interoperability. To try direct C++ imports later, add:  swiftSettings: [.interoperabilityMode(.Cxx)]
      // (then every module this target imports is parsed as C++, and DeckBridge.h still imports unchanged).
      linkerSettings: [
        .linkedFramework("AppKit"), .linkedFramework("Metal"), .linkedFramework("MetalKit"), .linkedFramework("CoreText"),
        .linkedFramework("QuartzCore"), .linkedFramework("WebKit"), .linkedFramework("UserNotifications"),
      ]
    ),
  ],
  cxxLanguageStandard: .cxx17
)
