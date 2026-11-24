// mac/Sources/ClaudeDeck/Edition.swift - which build this is: the public build (the Swift twin of
// core/deck/edition.hpp).
enum Edition {
  static let isPublic = true
  // the names (core/deck/edition.hpp DECK_EDITION_PRODUCT; the C++ seams read the state folder from there)
  static let productName = "scroDeck"
  static let bundleId = "io.github.scroton.scrodeck"     // DECK_EDITION_MAC_BUNDLE_ID (mac/tools/make-app.sh writes it)
}
