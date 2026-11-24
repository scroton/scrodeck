// core/deck/version.hpp — THE ONE PLACE the native deck's version is written (windows/PACKAGING.md "Requests to
// the exe's owners" 2: "a version in one place").
//
// Plain #defines first, so a resource script can include this file too (windres / rc.exe define RC_INVOKED and
// must not see the C++ below):
//     #include "../../core/deck/version.hpp"
//     FILEVERSION CLAUDE_DECK_NATIVE_VERSION_COMMAS          PRODUCTVERSION CLAUDE_DECK_NATIVE_VERSION_COMMAS
//     VALUE "FileVersion", CLAUDE_DECK_NATIVE_VERSION        VALUE "ProductVersion", CLAUDE_DECK_NATIVE_VERSION_LABEL
// To release a new version change the two lines below and nothing else.
#pragma once

#define CLAUDE_DECK_NATIVE_VERSION "0.1.0"
#define CLAUDE_DECK_NATIVE_VERSION_COMMAS 0,1,0,0
// what the deck calls itself where the Electron deck's version could stand beside it (package.json's is the
// Electron app's): deck-state.json `deck.version`, the hello
#define CLAUDE_DECK_NATIVE_VERSION_LABEL CLAUDE_DECK_NATIVE_VERSION "-native"

#ifndef RC_INVOKED
namespace deck { namespace core {
constexpr const char* kNativeDeckNumber = CLAUDE_DECK_NATIVE_VERSION;          // "0.1.0": the exe's FileVersion, the package's name
constexpr const char* kNativeDeckVersion = CLAUDE_DECK_NATIVE_VERSION_LABEL;   // "0.1.0-native": DeckOptions::version
}}  // namespace deck::core
#endif
