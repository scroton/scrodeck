'use strict';
// The grid layout model: which tile occupies which block of a configurable R×C grid, and what happens to
// everyone else when a tile is placed. docs/NATIVE-PORT.md §3 F1–F4, rulings R1 R3 R4 R6 R7 R15 R16.
//
// This is the MODEL and nothing else — no DOM, no Electron, no pixels except as fractions. It loads two
// ways: as a classic script in the renderer (window.Layout) and with require() under node, so
// `node test/layout-unit.js` exercises every rule without a deck, and the C++ core ports rules rather
// than re-deriving them from renderer code.
//
// R16 (2026-09-06): the grid is user-configurable, independent rows × cols, default 2×2. It is ONE
// global setting; the user picks the shape and assigns bounding boxes to suit their monitors. The ceiling
// was 3×3 (nine cells, the digit keys) until the user raised it to 4×4 on 2026-10-05; the cells past 9 are
// reached by the letter block under the digits (renderer/js/85-focus.js). Cells are numbered in READING
// ORDER —
//
//     1 2 3     ← a 3×3 grid; a 2×2 is just 1 2 / 3 4, which is why everything 2×2 is unchanged
//     4 5 6
//     7 8 9
//
// A tile occupies a RECTANGULAR block of cells, stored as the sorted list of its cell numbers (plain, so
// it round-trips through tiles.json). Every model function takes the current `grid` = { rows, cols }.
(function (root, factory) {
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Layout = api;
})(typeof self !== 'undefined' ? self : this, function () {
  // clamp the grid to what is supported (1..MAX each way); default 2×2
  const MAX = 4;
  function dims(grid) {
    const one = v => { const n = Number(v); return Number.isFinite(n) ? Math.max(1, Math.min(MAX, Math.round(n))) : 2; };
    const R = one(grid && grid.rows), C = one(grid && grid.cols);
    return { R, C, N: R * C };
  }
  const rc = (k, C) => ({ r: Math.floor((k - 1) / C), c: (k - 1) % C });
  const cell = (r, c, C) => r * C + c + 1;

  function norm(set, grid) {
    const { N } = dims(grid);
    return [...new Set((set || []).map(Number).filter(k => Number.isInteger(k) && k >= 1 && k <= N))].sort((a, b) => a - b);
  }
  const key = (set, grid) => norm(set, grid).join(',');
  const same = (a, b, grid) => key(a, grid) === key(b, grid);

  // the bounding box of a cell set, in grid coordinates
  function bbox(set, grid) {
    const { C } = dims(grid);
    const s = norm(set, grid);
    if (!s.length) return null;
    let r0 = Infinity, r1 = -Infinity, c0 = Infinity, c1 = -Infinity;
    for (const k of s) { const p = rc(k, C); r0 = Math.min(r0, p.r); r1 = Math.max(r1, p.r); c0 = Math.min(c0, p.c); c1 = Math.max(c1, p.c); }
    return { r0, r1, c0, c1 };
  }
  function fill(box, grid) {
    const { C } = dims(grid);
    const out = [];
    for (let r = box.r0; r <= box.r1; r++) for (let c = box.c0; c <= box.c1; c++) out.push(cell(r, c, C));
    return out.sort((a, b) => a - b);
  }
  // a set is legal when it is exactly its own bounding rectangle — a filled block, no gaps
  function isLegal(set, grid) {
    const s = norm(set, grid);
    if (!s.length) return false;
    return key(s, grid) === key(fill(bbox(s, grid), grid), grid);
  }
  const fullSet = grid => { const { R, C } = dims(grid); return fill({ r0: 0, r1: R - 1, c0: 0, c1: C - 1 }, grid); };

  // R1 generalised: whatever cells are held (a digit chord, a drag), take their BOUNDING RECTANGLE. So a
  // diagonal pair fills to the block it spans, and holding opposite corners of the grid gives the whole
  // grid. Empty selection -> null (the caller decides).
  function legalize(set, grid) {
    const s = norm(set, grid);
    if (!s.length) return null;
    return fill(bbox(s, grid), grid);
  }

  // Geometry as fractions of the terminal area: { x, y, w, h } in [0, 1].
  function rect(set, grid) {
    const { R, C } = dims(grid);
    const b = bbox(set, grid);
    if (!b) return null;
    return { x: b.c0 / C, y: b.r0 / R, w: (b.c1 - b.c0 + 1) / C, h: (b.r1 - b.r0 + 1) / R };
  }

  const subset = (o, s) => o.every(k => s.includes(k));
  const overlaps = (o, s) => o.some(k => s.includes(k));

  // F3 / R3 generalised: tile `id` takes set S. Each other placed tile U with cells O resolves as
  //   O ⊆ S           -> evicted (R6)
  //   O ∩ S ≠ ∅       -> shrinks to the largest rectangular strip of O clear of S (R3: prefer width)
  //   O ∩ S = ∅       -> untouched
  function shrink(O, S, grid) {
    const o = bbox(O, grid), s = bbox(S, grid);
    if (!o || !s) return null;
    // the intersection of two rectangles is a rectangle
    const ir0 = Math.max(o.r0, s.r0), ir1 = Math.min(o.r1, s.r1), ic0 = Math.max(o.c0, s.c0), ic1 = Math.min(o.c1, s.c1);
    if (ir0 > ir1 || ic0 > ic1) return norm(O, grid);      // no real overlap: unchanged
    const strips = [
      { r0: o.r0, r1: ir0 - 1, c0: o.c0, c1: o.c1 },        // above the bite
      { r0: ir1 + 1, r1: o.r1, c0: o.c0, c1: o.c1 },        // below
      { r0: o.r0, r1: o.r1, c0: o.c0, c1: ic0 - 1 },        // left
      { r0: o.r0, r1: o.r1, c0: ic1 + 1, c1: o.c1 },        // right
    ].filter(b => b.r0 <= b.r1 && b.c0 <= b.c1);
    if (!strips.length) return null;                         // O wholly inside S -> caller evicts
    // largest by cell count; tie -> wider (more columns) — terminals want width; then topmost, leftmost
    strips.sort((a, b) => {
      const areaA = (a.r1 - a.r0 + 1) * (a.c1 - a.c0 + 1), areaB = (b.r1 - b.r0 + 1) * (b.c1 - b.c0 + 1);
      if (areaB !== areaA) return areaB - areaA;
      const wA = a.c1 - a.c0, wB = b.c1 - b.c0; if (wB !== wA) return wB - wA;
      if (a.r0 !== b.r0) return a.r0 - b.r0;
      return a.c0 - b.c0;
    });
    return fill(strips[0], grid);
  }

  function place(layout, id, set, grid) {
    const S = legalize(set, grid);
    if (!S) return { layout: { ...(layout || {}) }, evicted: [], shrunk: [] };
    const next = {}, evicted = [], shrunk = [];
    for (const [uid, O] of Object.entries(layout || {})) {
      if (uid === id) continue;
      const o = norm(O, grid);
      if (!overlaps(o, S)) { next[uid] = o; continue; }
      if (subset(o, S)) { evicted.push(uid); continue; }
      const r = shrink(o, S, grid);
      if (r && r.length) { next[uid] = r; shrunk.push({ id: uid, from: o, to: r }); }
      else evicted.push(uid);
    }
    next[id] = S;
    return { layout: next, evicted, shrunk };
  }
  function unplace(layout, id) { const next = { ...(layout || {}) }; delete next[id]; return next; }
  function prune(layout, liveIds, grid) {
    const live = new Set(liveIds || []);
    const out = {};
    for (const [id, set] of Object.entries(layout || {})) if (live.has(id) && isLegal(set, grid)) out[id] = norm(set, grid);
    return out;
  }
  const placedIds = layout => Object.keys(layout || {});
  const occupancy = (layout, id, grid) => (layout && layout[id]) ? norm(layout[id], grid) : null;
  const isEmpty = layout => !layout || !Object.keys(layout).length;

  // ---------------------------------------------------------------- the drop model
  // R15 (2×2 only): the X-split. Splitting a FULL 2×2 pane is by the diagonals (four triangles -> the four
  // halves); the dead centre takes the whole window; a half splits by a STRAIGHT line into its two
  // quadrants; a single quadrant is taken whole. Not recursive — triangles appear only on the full pane.
  // The 2×2 cells 1,2,3,4 are the reading-order cells, so this is unchanged from the quadrant model.
  // A full 2x2 pane has NINE stable zones (2026-09-08 — corners sat exactly on the diagonals and
  // flickered between two halves): the dead centre takes the whole pane; each CORNER square takes that
  // quadrant; the four triangles between them take the halves. CORNER is the corner square's inner edge
  // as a distance from centre (0.28 -> the outer 22% of each axis).
  const CENTRE = 0.16;
  const CORNER = 0.28;
  const eq2 = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
  function directionalHalf(S, lx, ly) {
    const s = [...S].sort((a, b) => a - b);
    if (s.length <= 1) return s;
    const cx = lx - 0.5, cy = ly - 0.5;
    if (s.length === 4) {
      if (Math.abs(cx) < CENTRE && Math.abs(cy) < CENTRE) return s;                       // centre -> whole
      if (Math.abs(cx) >= CORNER && Math.abs(cy) >= CORNER) {                            // a corner -> that quadrant
        return cy < 0 ? (cx < 0 ? [1] : [2]) : (cx < 0 ? [3] : [4]);
      }
      if (Math.abs(cy) >= Math.abs(cx)) return cy < 0 ? [1, 2] : [3, 4];                 // top / bottom triangle -> half
      return cx < 0 ? [1, 3] : [2, 4];                                                   // left / right triangle -> half
    }
    // A half: the same small centre SQUARE takes it whole (it used to be a band across the middle third
    // of the long axis, and a drop anywhere in it evicted the sitting tile — 2026-09-08); outside
    // the square, a straight split along the half's long axis.
    const centre = Math.abs(cx) < CENTRE && Math.abs(cy) < CENTRE;
    if (eq2(s, [1, 2]) || eq2(s, [3, 4])) {
      if (centre) return s;
      return eq2(s, [1, 2]) ? (cx < 0 ? [1] : [2]) : (cx < 0 ? [3] : [4]);
    }
    if (centre) return s;
    return eq2(s, [1, 3]) ? (cy < 0 ? [1] : [3]) : (cy < 0 ? [2] : [4]);
  }
  // A tile that is ALREADY placed is dropped on `set` (2026-09-08): inside its own block
  // it takes that part (shrink); on a region that joins its block into a rectangle it takes the union
  // (grow — "expand into the new position"); anywhere else it moves there. An unplaced tile just takes `set`.
  function resolveDrop(layout, id, set, grid) {
    const s = norm(set, grid);
    const own = occupancy(layout, id, grid);
    if (!own || !own.length) return s;
    if (s.every(k => own.includes(k))) return s;
    const u = norm([...own, ...s], grid);
    return isLegal(u, grid) ? u : s;
  }
  // The grid shape changes (2026-09-10: "new panes appear unoccupied" — the sessions already placed
  // stay where they are). Every block keeps its rows and columns; a block that no longer fits (the grid
  // shrank under it) is dropped back to the sidebar. Cells are renumbered for the new grid. The new cells
  // are simply empty — to the right and below what was there.
  function regrid(layout, fromGrid, toGrid) {
    const out = {};
    const to = dims(toGrid);
    for (const [id, set] of Object.entries(layout || {})) {
      const s = norm(set, fromGrid); if (!s.length) continue;
      const b = bbox(s, fromGrid);
      if (b.r1 >= to.R || b.c1 >= to.C) continue;
      out[id] = fill(b, toGrid);
    }
    return out;
  }
  // A point near where grid lines MEET takes every cell meeting there (2026-09-08): the four
  // around an interior crossing, the two either side of a line where it reaches the outer border. The
  // block is cells, not panes — on a 3-row grid the crossing under the top two rows takes those four
  // and leaves the bottom row alone; whoever sits on those cells then follows the conflict rules. Within
  // JUNCTION of a cell's size on both axes; an outer corner meets one cell and is not a junction.
  const JUNCTION = 0.2;
  function junctionAt(px, py, grid) {
    const { R, C } = dims(grid);
    const gx = px * C, gy = py * R;
    const cv = Math.round(gx), rh = Math.round(gy);
    if (Math.abs(gx - cv) >= JUNCTION || Math.abs(gy - rh) >= JUNCTION) return null;
    const cols = [cv - 1, cv].filter(c => c >= 0 && c < C);
    const rows = [rh - 1, rh].filter(r => r >= 0 && r < R);
    if (cols.length * rows.length < 2) return null;
    const out = []; for (const r of rows) for (const c of cols) out.push(cell(r, c, C));
    return out.sort((a, b) => a - b);
  }
  // the single cell a point falls in
  function cellAt(px, py, grid) {
    const { R, C } = dims(grid);
    const c = Math.min(C - 1, Math.max(0, Math.floor(Math.min(1, Math.max(0, px)) * C)));
    const r = Math.min(R - 1, Math.max(0, Math.floor(Math.min(1, Math.max(0, py)) * R)));
    return [cell(r, c, C)];
  }
  // What a drop at (px, py) — fractions of the whole area — takes, given the layout and grid.
  //   empty layout                 -> the whole grid (first placement)
  //   near a grid junction         -> every cell meeting there (a 2×2 block, or a pair at the border)
  //   2×2, over a placed pane       -> the triangle / straight split of that pane (R15)
  //   otherwise                     -> the single cell under the cursor (the digit chord spans blocks)
  function dropTarget(layout, px, py, grid) {
    const { R, C } = dims(grid);
    const x = Math.min(1, Math.max(0, px)), y = Math.min(1, Math.max(0, py));
    if (isEmpty(layout)) return fullSet(grid);
    const j = junctionAt(x, y, grid); if (j) return j;
    if (R === 2 && C === 2) {
      for (const [id, set] of Object.entries(layout)) {
        const r = rect(set, grid); if (!r) continue;
        if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
          return directionalHalf(norm(set, grid), (x - r.x) / r.w, (y - r.y) / r.h);
        }
      }
    }
    return cellAt(x, y, grid);
  }

  return {
    MAX, dims, cell, rc, norm, key, same, bbox, fill, isLegal, fullSet, legalize, rect,
    shrink, place, unplace, prune, placedIds, occupancy, isEmpty,
    directionalHalf, junctionAt, cellAt, dropTarget, resolveDrop, regrid,
  };
});
