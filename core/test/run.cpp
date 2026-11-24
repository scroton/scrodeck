// core/test/run.cpp — checks the C++ core against the same cases the JS unit tests use. It does NOT parse
// the JSON fixtures (no JSON lib pulled in); it encodes the same input→output vectors directly, so the
// binary is self-contained. The fixtures remain the language-neutral oracle; this file is the C++ port of
// the layout-unit.js checks, and the two agree by construction (both come from lib/layout.js).
//
//   g++ -std=c++17 core/model/layout.cpp core/test/run.cpp -o run && ./run
#include "../model/layout.hpp"
#include "../model/hibernate.hpp"
#include "../model/prompts.hpp"
#include "../model/triggers.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace deck::layout;

static int passed = 0, failed = 0;
static std::string show(const Cells& c) { std::string s = "{"; for (size_t i = 0; i < c.size(); ++i) { if (i) s += ","; s += std::to_string(c[i]); } return s + "}"; }
static void check(const char* name, bool ok, const std::string& got = "") {
  if (ok) { ++passed; }
  else { ++failed; std::printf("FAIL  %s%s%s\n", name, got.empty() ? "" : "  got ", got.c_str()); }
}
static bool eq(const Cells& a, std::initializer_list<int> b) { Cells v(b); return a == v; }
static bool eq(const Cells& a, const Cells& b) { return a == b; }

static const Grid G22 { 2, 2 }, G33 { 3, 3 }, G13 { 1, 3 }, G23 { 2, 3 }, G44 { 4, 4 };

int main() {
  // legality / fullSet / legalize (R1 generalised)
  check("2x2 full is 1..4", eq(fullSet(G22), { 1, 2, 3, 4 }));
  check("3x3 full is 1..9", eq(fullSet(G33), { 1, 2, 3, 4, 5, 6, 7, 8, 9 }));
  check("1x3 full is 1..3", eq(fullSet(G13), { 1, 2, 3 }));
  check("isLegal 2x2 block on 3x3", isLegal({ 1, 2, 4, 5 }, G33));
  check("isLegal rejects a gap", !isLegal({ 1, 3 }, G33));
  check("isLegal rejects an L", !isLegal({ 1, 2, 4 }, G33));
  check("R1 2x2 diagonal -> full", eq(legalize({ 1, 4 }, G22), { 1, 2, 3, 4 }));
  check("R1 3x3 opposite corners -> whole", eq(legalize({ 1, 9 }, G33), { 1, 2, 3, 4, 5, 6, 7, 8, 9 }));
  check("3x3 hold 1+5 -> top-left 2x2", eq(legalize({ 1, 5 }, G33), { 1, 2, 4, 5 }));
  check("3x3 hold 4+6 -> middle row", eq(legalize({ 4, 6 }, G33), { 4, 5, 6 }));

  // geometry
  auto rEq = [](Rect r, double x, double y, double w, double h) { return r.x == x && r.y == y && r.w == w && r.h == h; };
  check("rect top-left 2x2", rEq(rect({ 1 }, G22), 0, 0, 0.5, 0.5));
  check("rect top half 2x2", rEq(rect({ 1, 2 }, G22), 0, 0, 1, 0.5));
  check("rect centre cell 3x3", rEq(rect({ 5 }, G33), 1.0 / 3, 1.0 / 3, 1.0 / 3, 1.0 / 3));
  check("rect top-right 3x3", rEq(rect({ 3 }, G33), 2.0 / 3, 0, 1.0 / 3, 1.0 / 3));
  check("rect bottom-right 2x3", rEq(rect({ 6 }, G23), 2.0 / 3, 0.5, 1.0 / 3, 0.5));

  // F3 / R3 / R6
  { auto r = place({ { "a", { 1, 2, 3, 4 } } }, "b", { 1 }, G22);
    check("R3: 2x2 full losing {1} -> bottom half", eq(occupancy(r.layout, "a", G22), { 3, 4 }) && eq(occupancy(r.layout, "b", G22), { 1 }) && r.evicted.empty() && r.shrunk.size() == 1); }
  { auto r = place({ { "a", { 1, 2, 3, 4 } } }, "b", { 3 }, G22);
    check("R3: losing {3} -> top half", eq(occupancy(r.layout, "a", G22), { 1, 2 })); }
  { auto r = place({ { "a", { 1 } } }, "b", { 1, 2 }, G22);
    check("R6: covered tile evicted", r.evicted.size() == 1 && r.evicted[0] == "a" && occupancy(r.layout, "a", G22).empty()); }
  { auto r = place({ { "a", fullSet(G33) } }, "b", { 5 }, G33);
    Cells a = occupancy(r.layout, "a", G33);
    bool clear = std::find(a.begin(), a.end(), 5) == a.end();
    check("3x3 full loses centre: b={5}, a shrinks legal & clear of 5", eq(occupancy(r.layout, "b", G33), { 5 }) && isLegal(a, G33) && clear, show(a)); }
  { auto r = place({ { "a", { 1, 2, 4, 5 } }, { "b", { 3 } } }, "c", { 2, 5 }, G33);
    Cells a = occupancy(r.layout, "a", G33);
    bool noBite = std::find(a.begin(), a.end(), 2) == a.end() && std::find(a.begin(), a.end(), 5) == a.end();
    check("3x3 partial overlap shrinks, disjoint kept", eq(occupancy(r.layout, "c", G33), { 2, 5 }) && noBite && eq(occupancy(r.layout, "b", G33), { 3 })); }
  check("shrink full 3x3 by {5} -> largest clear strip", eq(shrink(fullSet(G33), { 5 }, G33), { 1, 2, 3 }));
  { LayoutMap in { { "a", { 1 } } }; place(in, "b", { 2 }, G22); check("place does not mutate input", in.size() == 1 && eq(occupancy(in, "a", G22), { 1 })); }

  // drop model
  check("R15 2x2 full top -> top half", eq(dropTarget({ { "a", { 1, 2, 3, 4 } } }, 0.5, 0.1, G22), { 1, 2 }));
  check("R15 2x2 full centre -> whole", eq(dropTarget({ { "a", { 1, 2, 3, 4 } } }, 0.5, 0.5, G22), { 1, 2, 3, 4 }));
  check("R15 2x2 top half left -> {1}", eq(dropTarget({ { "a", { 1, 2 } } }, 0.1, 0.25, G22), { 1 }));
  check("R15 tall half top -> {1}", eq(directionalHalf({ 1, 3 }, 0.5, 0.1), { 1 }));
  check("R15 wide half right -> {2}", eq(directionalHalf({ 1, 2 }, 0.9, 0.5), { 2 }));
  check("R15 quadrant taken whole", eq(directionalHalf({ 4 }, 0.1, 0.1), { 4 }));
  check("R15 full pane corners -> quadrants", eq(directionalHalf({ 1, 2, 3, 4 }, 0.05, 0.05), { 1 }) && eq(directionalHalf({ 1, 2, 3, 4 }, 0.95, 0.05), { 2 }) && eq(directionalHalf({ 1, 2, 3, 4 }, 0.05, 0.95), { 3 }) && eq(directionalHalf({ 1, 2, 3, 4 }, 0.95, 0.95), { 4 }));
  check("R15 edge midpoints still halves", eq(directionalHalf({ 1, 2, 3, 4 }, 0.5, 0.05), { 1, 2 }) && eq(directionalHalf({ 1, 2, 3, 4 }, 0.05, 0.5), { 1, 3 }));
  check("half: centre square -> whole", eq(directionalHalf({ 1, 2 }, 0.5, 0.5), { 1, 2 }) && eq(directionalHalf({ 1, 3 }, 0.5, 0.5), { 1, 3 }));
  check("half: mid-axis near an edge -> a split", eq(directionalHalf({ 1, 2 }, 0.45, 0.06), { 1 }) && eq(directionalHalf({ 1, 2 }, 0.55, 0.94), { 2 }) && eq(directionalHalf({ 1, 3 }, 0.06, 0.45), { 1 }) && eq(directionalHalf({ 3, 4 }, 0.45, 0.06), { 3 }));
  {
    LayoutMap r = regrid({ { "a", { 1 } }, { "b", { 2 } }, { "c", { 3, 4 } } }, G22, G33);
    check("regrid 2x2 -> 3x3 keeps rows/cols", r.size() == 3 && eq(r[0].second, { 1 }) && eq(r[1].second, { 2 }) && eq(r[2].second, { 4, 5 }));
    LayoutMap r2 = regrid({ { "a", { 1 } }, { "b", { 3 } }, { "c", { 4, 5 } }, { "d", { 9 } } }, G33, G22);
    check("regrid 3x3 -> 2x2 drops what no longer fits", r2.size() == 2 && r2[0].first == "a" && eq(r2[0].second, { 1 }) && r2[1].first == "c" && eq(r2[1].second, { 3, 4 }));
    check("regrid identity", eq(regrid({ { "a", { 1, 2, 3, 4 } } }, G22, G22)[0].second, { 1, 2, 3, 4 }));
  }
  check("resolveDrop unplaced -> set", eq(resolveDrop({ { "x", { 1, 2 } } }, "y", { 3 }, G22), { 3 }));
  check("resolveDrop inside own block -> shrink", eq(resolveDrop({ { "y", { 1, 2, 3, 4 } } }, "y", { 1, 2 }, G22), { 1, 2 }));
  check("resolveDrop rectangle -> grow", eq(resolveDrop({ { "y", { 1 } } }, "y", { 2 }, G22), { 1, 2 }) && eq(resolveDrop({ { "y", { 4 } } }, "y", { 2 }, G22), { 2, 4 }) && eq(resolveDrop({ { "y", { 3, 4 } } }, "y", { 1, 2 }, G22), { 1, 2, 3, 4 }));
  check("resolveDrop no rectangle -> move", eq(resolveDrop({ { "y", { 1 } } }, "y", { 4 }, G22), { 4 }) && eq(resolveDrop({ { "y", { 3, 4 } } }, "y", { 1 }, G22), { 1 }));
  check("resolveDrop 3x3 row grows, gap moves", eq(resolveDrop({ { "y", { 1, 2 } } }, "y", { 3 }, G33), { 1, 2, 3 }) && eq(resolveDrop({ { "y", { 1 } } }, "y", { 3 }, G33), { 3 }));
  {
    LayoutMap split = { { "a", { 1 } }, { "b", { 2 } }, { "c", { 3, 4 } } };
    check("junction: split 2x2 centre crossing -> whole", eq(dropTarget(split, 0.5, 0.5, G22), { 1, 2, 3, 4 }));
    check("junction: top border pair / left border pair", eq(dropTarget(split, 0.5, 0.04, G22), { 1, 2 }) && eq(dropTarget(split, 0.04, 0.5, G22), { 1, 3 }));
    check("junction: inside a quadrant unchanged", eq(dropTarget(split, 0.25, 0.25, G22), { 1 }) && eq(dropTarget(split, 0.45, 0.35, G22), { 1 }));
    check("junction: outer corner is not one", junctionAt(0.02, 0.02, G22).empty() && eq(dropTarget({ { "a", { 1, 2, 3, 4 } } }, 0.02, 0.02, G22), { 1 }));
    check("junction: zone 0.2 cell", eq(junctionAt(0.59, 0.41, G22), { 1, 2, 3, 4 }) && junctionAt(0.61, 0.5, G22).empty());
    Grid G32 { 3, 2 };
    check("junction: 3 rows, top crossing -> top four", eq(dropTarget({ { "a", fullSet(G32) } }, 0.5, 1.0 / 3, G32), { 1, 2, 3, 4 }));
    check("junction: 2x3 crossing -> {1,2,4,5}", eq(dropTarget({ { "a", fullSet(G23) } }, 1.0 / 3, 0.5, G23), { 1, 2, 4, 5 }));
    check("junction: 3x3 crossing and border pairs", eq(dropTarget({ { "a", fullSet(G33) } }, 2.0 / 3, 2.0 / 3, G33), { 5, 6, 8, 9 }) && eq(dropTarget({ { "a", fullSet(G33) } }, 1.0 / 3, 0.02, G33), { 1, 2 }) && eq(dropTarget({ { "a", fullSet(G33) } }, 0.98, 2.0 / 3, G33), { 6, 9 }));
  }
  check("R16 3x3 centre drop -> cell 5", eq(dropTarget({ { "a", fullSet(G33) } }, 0.5, 0.5, G33), { 5 }));
  check("R16 3x3 top-right drop -> cell 3", eq(dropTarget({ { "a", fullSet(G33) } }, 0.9, 0.1, G33), { 3 }));
  check("empty layout -> whole grid", eq(dropTarget({}, 0.5, 0.5, G33), fullSet(G33)));
  check("1x3 right third -> cell 3", eq(cellAt(0.9, 0.5, G13), { 3 }));

  // plumbing
  check("norm drops out-of-range, dedupes, sorts", eq(norm({ 4, 1, 4, 9, 0, 2 }, G22), { 1, 2, 4 }));
  check("dims clamps to 1..4", dims({ 0, 9 }).R == 1 && dims({ 0, 9 }).C == 4 && dims({ 5, 5 }).N == 16);

  // up to 4x4 (2026-10-05)
  check("4x4 full is 1..16", fullSet(G44).size() == 16 && fullSet(G44).back() == 16);
  check("4x4 hold 6+11 -> centre block", eq(legalize({ 6, 11 }, G44), { 6, 7, 10, 11 }));
  check("4x4 rect cell 16", rEq(rect({ 16 }, G44), 0.75, 0.75, 0.25, 0.25));
  check("4x4 centre drop -> the four cells meeting there", eq(dropTarget({ { "a", fullSet(G44) } }, 0.5, 0.5, G44), { 6, 7, 10, 11 }));
  check("4x4 drop in a cell -> cell 16", eq(dropTarget({ { "a", fullSet(G44) } }, 0.9, 0.9, G44), { 16 }));
  { auto r = place({ { "a", fullSet(G44) } }, "b", { 6, 7, 10, 11 }, G44);
    check("4x4 full loses the centre block -> top row", eq(occupancy(r.layout, "a", G44), { 1, 2, 3, 4 })); }
  { auto r = regrid({ { "a", { 1 } }, { "b", { 5, 6, 8, 9 } } }, G33, G44);
    check("regrid 3x3 -> 4x4 keeps rows/cols", r.size() == 2 && eq(r[1].second, { 6, 7, 10, 11 })); }
  { auto r = regrid({ { "a", { 1 } }, { "b", { 6, 7, 10, 11 } }, { "d", { 16 } } }, G44, G33);
    check("regrid 4x4 -> 3x3 drops what no longer fits", r.size() == 2 && r[0].first == "a" && r[1].first == "b" && eq(r[1].second, { 5, 6, 8, 9 })); }
  check("same is order-insensitive", same({ 2, 1 }, { 1, 2 }, G22));
  { auto p = prune({ { "a", { 1 } }, { "gone", { 2 } }, { "bad", { 1, 3 } } }, { "a", "bad" }, G33);
    check("prune drops dead + illegal", p.size() == 1 && p[0].first == "a"); }

  // ---------------------------------------------------------------- hibernate
  {
    using namespace deck::hibernate;
    const int64_t NOW = 1700000000000LL, MIN = 60000;
    Config on; on.enabled = true; on.idleMinutes = 60;
    auto tile = []() { TileView t; t.id = "a"; t.kind = "claude"; t.sessionId = "s1"; t.resumable = true; t.status = "idle"; t.running = true; t.lastActivityAt = 1700000000000LL - 120 * 60000; return t; };
    check("hib: quiet idle session may sleep", sleepBlocker(tile(), NOW, on).empty());
    { TileView t = tile(); t.status = "busy"; check("hib: busy blocked", sleepBlocker(t, NOW, on) == "status: busy"); }
    { TileView t = tile(); t.pinned = true; check("hib: pinned blocked", sleepBlocker(t, NOW, on) == "pinned"); }
    { TileView t = tile(); t.shownBy = {"win:0"}; check("hib: on screen blocked", sleepBlocker(t, NOW, on) == "on screen (win:0)"); }
    { TileView t = tile(); t.shownBy = {"win:0","remote"}; check("hib: on-screen reason joins", sleepBlocker(t, NOW, on) == "on screen (win:0, remote)"); }
    { TileView t = tile(); t.held = 1; check("hib: holding a prompt blocked", sleepBlocker(t, NOW, on) == "holding a prompt"); }
    { TileView t = tile(); t.resumable = false; check("hib: never-prompted blocked", sleepBlocker(t, NOW, on) == "nothing to resume yet"); }
    { TileView t = tile(); t.lastActivityAt = NOW - 10 * MIN; check("hib: active recently blocked", sleepBlocker(t, NOW, on).rfind("active ", 0) == 0); }
    check("hib: maySleep true", maySleep(tile(), NOW, on));
    { TileView t = tile(); t.asleep = true; t.shownBy = {"win:1"}; check("hib: wakeReason on screen", wakeReason(t) == "on screen (win:1)"); }
    { TileView t = tile(); t.asleep = true; t.held = 2; check("hib: wakeReason due prompt", wakeReason(t) == "a prompt is waiting for it"); }
    { TileView t = tile(); t.asleep = true; check("hib: wakeReason none", wakeReason(t).empty()); }
    {
      TileView q3 = tile(); q3.id = "q3"; q3.lastActivityAt = NOW - 180 * MIN;
      TileView q4 = tile(); q4.id = "q4"; q4.lastActivityAt = NOW - 240 * MIN;
      TileView bs = tile(); bs.id = "busy"; bs.status = "busy";
      Config cap = on; cap.maxPerTick = 1;
      auto picks = chooseSleepers({ q3, q4, bs }, NOW, cap);
      check("hib: chooseSleepers longest-quiet first, capped", picks.size() == 1 && picks[0].id == "q4");
      // memory pressure: below the threshold nothing; at it, every eligible one, cap and idle wait waived, busy still kept
      check("hib: pressure below threshold -> none", pressureSleepers({ q3, q4, bs }, NOW, cap, 0.85).empty());
      auto pp = pressureSleepers({ q3, q4, bs }, NOW, cap, 0.93);
      check("hib: pressure -> q4 then q3, busy kept", pp.size() == 2 && pp[0].id == "q4" && pp[1].id == "q3");
      { TileView fresh = q3; fresh.id = "fresh"; fresh.lastActivityAt = NOW - 10 * MIN; Config off = cap; off.enabled = false;
        auto p2 = pressureSleepers({ fresh }, NOW, off, 0.95);
        check("hib: pressure waives the idle wait and the off switch", p2.size() == 1 && p2[0].id == "fresh"); }
      { TileView shown = q3; shown.id = "shown"; shown.asleep = true; shown.shownBy = { "win:0" }; check("hib: under pressure on screen does not wake, a due prompt does", wakeReason(shown, true).empty() && !wakeReason(shown, false).empty() && pressureMode(0.9, false) && pressureMode(0.87, true) && !pressureMode(0.84, true)); }
    }
    {
      TileView w = tile(); w.id = "w"; w.asleep = true; w.shownBy = {"win:0"};
      TileView x = tile(); x.id = "x"; x.asleep = true;
      auto wk = chooseWakers({ w, x });
      check("hib: chooseWakers only the wanted one", wk.size() == 1 && wk[0].id == "w");
    }
  }

  // ---------------------------------------------------------------- prompts (due-logic)
  {
    using namespace deck::prompts;
    check("prompt: inWindow inside", inWindow(600, 540, 660));
    check("prompt: inWindow outside", !inWindow(700, 540, 660));
    check("prompt: inWindow wraps midnight (in)", inWindow(30, 1380, 120));
    check("prompt: inWindow wraps midnight (out)", !inWindow(300, 1380, 120));
    check("prompt: zero-length window = any time", inWindow(0, 500, 500));
    auto risen = [](const std::string& n, void*) -> int64_t { return n == "ready" ? 5000 : 0; };
    const int64_t T = 10000;
    { Item i; i.kind = Kind::Timed; i.at = T - 1; check("prompt: timed at/after -> due", isDue(i, T, risen, nullptr)); }
    { Item i; i.kind = Kind::Timed; i.at = T + 1; check("prompt: timed before -> not due", !isDue(i, T, risen, nullptr)); }
    { Item i; i.kind = Kind::Next; i.at = 0; check("prompt: next -> due", isDue(i, T, risen, nullptr)); }
    { Item i; i.kind = Kind::Recurring; i.everyMin = 10; i.startMin = 0; i.endMin = 0; i.lastSentAt = 0; check("prompt: recurring any-time never sent -> due", isDue(i, T, risen, nullptr)); }
    { Item i; i.kind = Kind::Recurring; i.everyMin = 10; i.startMin = 0; i.endMin = 0; i.lastSentAt = T - 60000; check("prompt: recurring within interval -> not due", !isDue(i, T, risen, nullptr)); }
    { Item i; i.kind = Kind::Triggered; i.trigger = "ready"; i.firedAt = 0; check("prompt: triggered risen not fired -> due", isDue(i, T, risen, nullptr)); }
    { Item i; i.kind = Kind::Triggered; i.trigger = "ready"; i.firedAt = 5000; check("prompt: triggered already fired -> not due", !isDue(i, T, risen, nullptr)); }
    { Item i; i.kind = Kind::Triggered; i.trigger = "cold"; i.firedAt = 0; check("prompt: triggered false -> not due", !isDue(i, T, risen, nullptr)); }
    check("prompt: oneShot", oneShot(Kind::Timed) && oneShot(Kind::Next) && !oneShot(Kind::Recurring) && !oneShot(Kind::Triggered));
  }

  // ---------------------------------------------------------------- triggers (store + rising edge)
  {
    using namespace deck::triggers;
    check("trig: cleanName slug ok, junk rejected", cleanName("build.ok_1") == "build.ok_1" && cleanName("bad name").empty() && cleanName("../x").empty());
    Store st;
    const int64_t NOW = 1000000;
    st.set("x", true, "deck", NOW);
    check("trig: set true records edge", st.value("x") && st.risenAt("x") == NOW);
    st.set("x", true, "deck", NOW + 5000);
    check("trig: staying true keeps the edge", st.risenAt("x") == NOW);
    st.set("x", false, "deck", NOW + 6000);
    check("trig: false clears risen", !st.value("x") && st.risenAt("x") == 0);
    st.set("x", true, "deck", NOW + 7000);
    check("trig: fresh rising edge is new time", st.risenAt("x") == NOW + 7000);
    st.declare("y", "ui");
    check("trig: declare makes a false trigger", st.get("y") && st.get("y")->value == false && st.list().size() == 2);
    check("trig: remove", st.remove("y") && st.list().size() == 1);
  }

  std::printf("\n%d/%d passed\n", passed, passed + failed);
  return failed ? 1 : 0;
}
