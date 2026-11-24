// core/model/layout.hpp — the grid layout model, ported from lib/layout.js (docs/NATIVE-PORT.md §3, R1
// R3 R4 R6 R15 R16). PURE: no I/O, no UI, no threads. The C++ shells and the Swift shell both call this;
// it is the single place the placement rules live natively, exactly as lib/layout.js is the single place
// they live in the Electron app.
//
// STATUS: header only. There is no C++ compiler on the build box yet, so this declares the surface and
// documents the contract; the implementation (layout.cpp) is written against core/test/fixtures/layout.json,
// which is generated from lib/layout.js and is the byte-for-byte acceptance oracle. Do not hand-tune
// behaviour here — port the JS, then make the fixtures pass.
//
// Representation (must match the JS): a grid is R rows x C cols, each 1..kMax (4 since 2026-10-05; it was
// 3). Cells are numbered in READING order, 1..R*C (1 2 3 / 4 5 6 / 7 8 9). An occupancy is a SORTED set of cell numbers forming a filled
// rectangle. A layout maps a tile id -> its occupancy. Geometry is fractions of the terminal area in
// [0,1]. Every function takes the current grid.
#ifndef CLAUDE_DECK_CORE_LAYOUT_HPP
#define CLAUDE_DECK_CORE_LAYOUT_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace deck::layout {

constexpr int kMax = 4;                                 // the largest rows / cols (lib/layout.js MAX)
struct Grid { int rows = 2; int cols = 2; };            // clamped to 1..kMax by dims()
struct Dims { int R; int C; int N; };                   // rows, cols, cell count
struct Rect { double x, y, w, h; };                     // fractions of the area, [0,1]
struct BBox { int r0, r1, c0, c1; };                    // inclusive grid coordinates

using Cells  = std::vector<int>;                        // sorted, unique cell numbers
using TileId = std::string;
// PARITY TRAP: the JS layout is a plain object and place() iterates it in INSERTION order (Object.entries),
// which sets the order of `evicted`/`shrunk` below. A std::map iterates in SORTED-KEY order — different.
// The fixtures were generated with keys that happen to be sorted, so they do not catch the difference; a
// correct port must iterate in insertion order (keep a std::vector<std::pair<TileId,Cells>>, or a map plus
// an order vector). Do NOT use a bare std::map if evicted/shrunk order must match the JS.
using LayoutMap = std::vector<std::pair<TileId, Cells>>;

// place() result: the new layout plus who was displaced (R6 evicted, R3 shrunk)
struct ShrinkInfo { TileId id; Cells from; Cells to; };
struct PlaceResult { LayoutMap layout; std::vector<TileId> evicted; std::vector<ShrinkInfo> shrunk; };

// ---- grid + cells
Dims  dims(const Grid& g);                              // clamp; N = R*C
Cells norm(const Cells& set, const Grid& g);            // dedupe, sort, drop out-of-range
std::string key(const Cells& set, const Grid& g);       // canonical "1,2,4" for equality
bool  same(const Cells& a, const Cells& b, const Grid& g);
// (internal helpers cell(r,c,C) and rc(k,C) convert between a cell number and its row/col; not part of
// the public surface — inline them in the .cpp)
BBox  bbox(const Cells& set, const Grid& g);            // precondition: set non-empty
Cells fill(const BBox& box, const Grid& g);             // every cell in the rectangle, sorted
bool  isLegal(const Cells& set, const Grid& g);         // == its own bounding rectangle (filled)
Cells fullSet(const Grid& g);                           // the whole grid

// R1 generalised: whatever cells are held -> their bounding rectangle. Empty -> empty (caller decides).
Cells legalize(const Cells& set, const Grid& g);

// geometry
Rect  rect(const Cells& set, const Grid& g);            // precondition: legal, non-empty

// F3 / R3 / R6: place tile `id` at `set`; evict covered tiles, shrink partly-covered ones to the largest
// clear rectangular strip (preferring width). Does not mutate `in`.
PlaceResult place(const LayoutMap& in, const TileId& id, const Cells& set, const Grid& g);
Cells shrink(const Cells& occupied, const Cells& taken, const Grid& g);   // largest strip of `occupied` clear of `taken`
Cells occupancy(const LayoutMap& in, const TileId& id, const Grid& g);   // a tile's cells, or empty
std::vector<TileId> placedIds(const LayoutMap& in);                     // ids in map order
LayoutMap unplace(const LayoutMap& in, const TileId& id);
LayoutMap prune(const LayoutMap& in, const std::vector<TileId>& liveIds, const Grid& g);
bool  isEmpty(const LayoutMap& in);

// ---- the drop model
// 2x2 only (R15): the X-split. A full pane splits by diagonals into the four halves; the dead centre takes
// the whole window; a half splits by a straight line into its two quadrants; a quadrant is taken whole.
Cells directionalHalf(const Cells& paneSet, double lx, double ly);       // (lx,ly) local to the pane, [0,1]
Cells cellAt(double px, double py, const Grid& g);                        // the single cell under a point
// What a drop at (px,py) [fractions of the whole area] takes: empty layout -> whole grid; on 2x2 over a
// placed pane -> the R15 split of that pane; otherwise -> the cell under the cursor (R16).
// every cell meeting at the grid junction near (px,py), or empty when the point is not near one
Cells junctionAt(double px, double py, const Grid& g);
Cells dropTarget(const LayoutMap& in, double px, double py, const Grid& g);
// A placed tile dropped on `set`: inside its own block -> that part; joins into a rectangle -> the union; else `set`.
Cells resolveDrop(const LayoutMap& in, const TileId& id, const Cells& set, const Grid& g);
// a grid change: every block keeps its rows and columns (renumbered); blocks that no longer fit are dropped
LayoutMap regrid(const LayoutMap& in, const Grid& from, const Grid& to);

}  // namespace deck::layout

#endif  // CLAUDE_DECK_CORE_LAYOUT_HPP
