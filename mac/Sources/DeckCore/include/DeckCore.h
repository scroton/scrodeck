/* mac/Sources/DeckCore/include/DeckCore.h — nothing. SwiftPM wants a public header folder for a C-family target;
 * no Swift code imports DeckCore. The core's headers are included by path from the targets that need them
 * ("core/deck/deck.hpp", through `headerSearchPath("../DeckCore")`). The sources are `core` beside this folder: a
 * symlink to the repo's own core/ — the SAME files the Windows shell compiles, never a copy. */
