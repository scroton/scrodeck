// core/model/layout.cpp — implementation of the grid layout model, ported from lib/layout.js.
// Parity port: same operations in the same order, so the doubles from rect() match the JS exactly. The
// oracle is core/test/fixtures/layout.json (generated from the JS); core/test/run.cpp checks it.
#include "layout.hpp"

#include <algorithm>
#include <cmath>

namespace deck::layout {

Dims dims(const Grid& g) {
  auto one = [](int v) { if (v < 1) return 1; if (v > kMax) return kMax; return v; };
  int R = one(g.rows), C = one(g.cols);
  return { R, C, R * C };
}

// internal cell <-> (row,col), reading order, 1-based cells
static inline int rrow(int k, int C) { return (k - 1) / C; }
static inline int rcol(int k, int C) { return (k - 1) % C; }
static inline int cellOf(int r, int c, int C) { return r * C + c + 1; }

Cells norm(const Cells& set, const Grid& g) {
  int N = dims(g).N;
  Cells out;
  for (int k : set) if (k >= 1 && k <= N && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
  std::sort(out.begin(), out.end());
  return out;
}

std::string key(const Cells& set, const Grid& g) {
  Cells s = norm(set, g);
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) { if (i) out += ','; out += std::to_string(s[i]); }
  return out;
}
bool same(const Cells& a, const Cells& b, const Grid& g) { return key(a, g) == key(b, g); }

BBox bbox(const Cells& set, const Grid& g) {
  int C = dims(g).C;
  Cells s = norm(set, g);
  BBox b { 1 << 30, -(1 << 30), 1 << 30, -(1 << 30) };
  for (int k : s) {
    int r = rrow(k, C), c = rcol(k, C);
    b.r0 = std::min(b.r0, r); b.r1 = std::max(b.r1, r);
    b.c0 = std::min(b.c0, c); b.c1 = std::max(b.c1, c);
  }
  return b;
}

Cells fill(const BBox& box, const Grid& g) {
  int C = dims(g).C;
  Cells out;
  for (int r = box.r0; r <= box.r1; ++r) for (int c = box.c0; c <= box.c1; ++c) out.push_back(cellOf(r, c, C));
  std::sort(out.begin(), out.end());
  return out;
}

bool isLegal(const Cells& set, const Grid& g) {
  Cells s = norm(set, g);
  if (s.empty()) return false;
  return key(s, g) == key(fill(bbox(s, g), g), g);
}

Cells fullSet(const Grid& g) { auto d = dims(g); return fill({ 0, d.R - 1, 0, d.C - 1 }, g); }

Cells legalize(const Cells& set, const Grid& g) {
  Cells s = norm(set, g);
  if (s.empty()) return {};
  return fill(bbox(s, g), g);
}

Rect rect(const Cells& set, const Grid& g) {
  auto d = dims(g);
  BBox b = bbox(set, g);
  return { (double)b.c0 / d.C, (double)b.r0 / d.R, (double)(b.c1 - b.c0 + 1) / d.C, (double)(b.r1 - b.r0 + 1) / d.R };
}

static bool subset(const Cells& o, const Cells& s) {
  for (int k : o) if (std::find(s.begin(), s.end(), k) == s.end()) return false;
  return true;
}
static bool overlaps(const Cells& o, const Cells& s) {
  for (int k : o) if (std::find(s.begin(), s.end(), k) != s.end()) return true;
  return false;
}

Cells shrink(const Cells& O, const Cells& S, const Grid& g) {
  Cells no = norm(O, g), ns = norm(S, g);
  if (no.empty() || ns.empty()) return {};
  BBox o = bbox(no, g), s = bbox(ns, g);
  int ir0 = std::max(o.r0, s.r0), ir1 = std::min(o.r1, s.r1), ic0 = std::max(o.c0, s.c0), ic1 = std::min(o.c1, s.c1);
  if (ir0 > ir1 || ic0 > ic1) return no;   // no real overlap
  struct Strip { int r0, r1, c0, c1; };
  Strip cand[4] = {
    { o.r0, ir0 - 1, o.c0, o.c1 },   // above
    { ir1 + 1, o.r1, o.c0, o.c1 },   // below
    { o.r0, o.r1, o.c0, ic0 - 1 },   // left
    { o.r0, o.r1, ic1 + 1, o.c1 },   // right
  };
  std::vector<Strip> strips;
  for (auto& st : cand) if (st.r0 <= st.r1 && st.c0 <= st.c1) strips.push_back(st);
  if (strips.empty()) return {};
  std::stable_sort(strips.begin(), strips.end(), [](const Strip& a, const Strip& b) {
    int areaA = (a.r1 - a.r0 + 1) * (a.c1 - a.c0 + 1), areaB = (b.r1 - b.r0 + 1) * (b.c1 - b.c0 + 1);
    if (areaA != areaB) return areaA > areaB;              // larger first
    int wA = a.c1 - a.c0, wB = b.c1 - b.c0; if (wA != wB) return wA > wB;   // wider first
    if (a.r0 != b.r0) return a.r0 < b.r0;                  // topmost
    return a.c0 < b.c0;                                    // leftmost
  });
  return fill({ strips[0].r0, strips[0].r1, strips[0].c0, strips[0].c1 }, g);
}

static const Cells* findIn(const LayoutMap& m, const TileId& id) {
  for (auto& kv : m) if (kv.first == id) return &kv.second;
  return nullptr;
}

PlaceResult place(const LayoutMap& in, const TileId& id, const Cells& set, const Grid& g) {
  PlaceResult res;
  Cells S = legalize(set, g);
  if (S.empty()) { res.layout = in; return res; }
  for (auto& kv : in) {                     // insertion order (LayoutMap is a vector<pair>)
    if (kv.first == id) continue;
    Cells o = norm(kv.second, g);
    if (!overlaps(o, S)) { res.layout.push_back({ kv.first, o }); continue; }
    if (subset(o, S)) { res.evicted.push_back(kv.first); continue; }
    Cells r = shrink(o, S, g);
    if (!r.empty()) { res.layout.push_back({ kv.first, r }); res.shrunk.push_back({ kv.first, o, r }); }
    else res.evicted.push_back(kv.first);
  }
  res.layout.push_back({ id, S });
  return res;
}

LayoutMap unplace(const LayoutMap& in, const TileId& id) {
  LayoutMap out;
  for (auto& kv : in) if (kv.first != id) out.push_back(kv);
  return out;
}

Cells occupancy(const LayoutMap& in, const TileId& id, const Grid& g) {
  const Cells* c = findIn(in, id);
  return c ? norm(*c, g) : Cells{};
}
std::vector<TileId> placedIds(const LayoutMap& in) {
  std::vector<TileId> out;
  for (auto& kv : in) out.push_back(kv.first);
  return out;
}
LayoutMap prune(const LayoutMap& in, const std::vector<TileId>& liveIds, const Grid& g) {
  LayoutMap out;
  for (auto& kv : in) {
    bool live = std::find(liveIds.begin(), liveIds.end(), kv.first) != liveIds.end();
    if (live && isLegal(kv.second, g)) out.push_back({ kv.first, norm(kv.second, g) });
  }
  return out;
}
bool isEmpty(const LayoutMap& in) { return in.empty(); }

// ---- the drop model (R15 for 2x2, cell-under-cursor otherwise)
static const double CENTRE = 0.16;
static const double CORNER = 0.28;   // a full pane's corner squares take that quadrant (2026-09-08)
static bool cellsEq(const Cells& a, std::initializer_list<int> b) {
  if (a.size() != b.size()) return false;
  return std::equal(a.begin(), a.end(), b.begin());
}

Cells directionalHalf(const Cells& S, double lx, double ly) {
  Cells s = S; std::sort(s.begin(), s.end());
  if (s.size() <= 1) return s;
  double cx = lx - 0.5, cy = ly - 0.5;
  if (s.size() == 4) {
    if (std::fabs(cx) < CENTRE && std::fabs(cy) < CENTRE) return s;
    if (std::fabs(cx) >= CORNER && std::fabs(cy) >= CORNER)
      return cy < 0 ? (cx < 0 ? Cells{ 1 } : Cells{ 2 }) : (cx < 0 ? Cells{ 3 } : Cells{ 4 });
    if (std::fabs(cy) >= std::fabs(cx)) return cy < 0 ? Cells{ 1, 2 } : Cells{ 3, 4 };
    return cx < 0 ? Cells{ 1, 3 } : Cells{ 2, 4 };
  }
  // a half: the centre SQUARE takes it whole (2026-09-08; was a band along the long axis)
  const bool centre = std::fabs(cx) < CENTRE && std::fabs(cy) < CENTRE;
  if (cellsEq(s, { 1, 2 }) || cellsEq(s, { 3, 4 })) {
    if (centre) return s;
    if (cellsEq(s, { 1, 2 })) return cx < 0 ? Cells{ 1 } : Cells{ 2 };
    return cx < 0 ? Cells{ 3 } : Cells{ 4 };
  }
  if (centre) return s;
  if (cellsEq(s, { 1, 3 })) return cy < 0 ? Cells{ 1 } : Cells{ 3 };
  return cy < 0 ? Cells{ 2 } : Cells{ 4 };
}

Cells resolveDrop(const LayoutMap& in, const TileId& id, const Cells& set, const Grid& g) {
  Cells s = norm(set, g);
  Cells own = occupancy(in, id, g);
  if (own.empty()) return s;
  if (subset(s, own)) return s;
  Cells u = own; u.insert(u.end(), s.begin(), s.end()); u = norm(u, g);
  return isLegal(u, g) ? u : s;
}

LayoutMap regrid(const LayoutMap& in, const Grid& from, const Grid& to) {
  LayoutMap out;
  auto d = dims(to);
  for (auto& kv : in) {
    Cells s = norm(kv.second, from); if (s.empty()) continue;
    BBox b = bbox(s, from);
    if (b.r1 >= d.R || b.c1 >= d.C) continue;
    out.push_back({ kv.first, fill(b, to) });
  }
  return out;
}

static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// a point near where grid lines meet takes every cell meeting there (2026-09-08); JUNCTION of a cell on both axes
static const double JUNCTION = 0.2;
Cells junctionAt(double px, double py, const Grid& g) {
  auto d = dims(g);
  double gx = px * d.C, gy = py * d.R;
  double cv = std::round(gx), rh = std::round(gy);
  if (std::fabs(gx - cv) >= JUNCTION || std::fabs(gy - rh) >= JUNCTION) return {};
  std::vector<int> cols, rows;
  for (int c : { (int)cv - 1, (int)cv }) if (c >= 0 && c < d.C) cols.push_back(c);
  for (int r : { (int)rh - 1, (int)rh }) if (r >= 0 && r < d.R) rows.push_back(r);
  if (cols.size() * rows.size() < 2) return {};
  Cells out;
  for (int r : rows) for (int c : cols) out.push_back(cellOf(r, c, d.C));
  std::sort(out.begin(), out.end());
  return out;
}

Cells cellAt(double px, double py, const Grid& g) {
  auto d = dims(g);
  int c = (int)std::floor(clamp01(px) * d.C); if (c > d.C - 1) c = d.C - 1; if (c < 0) c = 0;
  int r = (int)std::floor(clamp01(py) * d.R); if (r > d.R - 1) r = d.R - 1; if (r < 0) r = 0;
  return { cellOf(r, c, d.C) };
}

Cells dropTarget(const LayoutMap& in, double px, double py, const Grid& g) {
  auto d = dims(g);
  double x = clamp01(px), y = clamp01(py);
  if (isEmpty(in)) return fullSet(g);
  Cells j = junctionAt(x, y, g); if (!j.empty()) return j;
  if (d.R == 2 && d.C == 2) {
    for (auto& kv : in) {
      Rect r = rect(kv.second, g);
      if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h)
        return directionalHalf(norm(kv.second, g), (x - r.x) / r.w, (y - r.y) / r.h);
    }
  }
  return cellAt(x, y, g);
}

}  // namespace deck::layout
