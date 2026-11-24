# Porting map: JS model → native core

Each pure `lib/*` model module becomes a C++ translation unit in `core/model/`. The JS module stays the
source of truth (it carries the unit tests); the C++ is a parity port checked against a fixture file
generated from the JS. **Behaviour parity is the whole point** — a ruling written against the Electron app
must hold identically in the native shells, or it is not one deck with three front ends.

Regenerate fixtures after any model change: `node core/test/gen-fixtures.js`.

| JS module | rules | C++ | fixtures | status |
|---|---|---|---|---|
| `lib/layout.js` | grid model, R1 R3 R4 R6 R15 R16 | `model/layout.{hpp,cpp}` | `layout.json` (34) | **done, tested** |
| `lib/hibernate.js` | sleep policy: test / triggers / decision | `model/hibernate.{hpp,cpp}` | `hibernate.json` (15) | **done, tested** |
| `lib/queue.js` | special-prompt due-logic (4 kinds) | `model/prompts.{hpp,cpp}` | `prompts.json` (13) | **done, tested** |
| `lib/triggers.js` | named booleans, rising-edge time | `model/triggers.{hpp,cpp}` | (sequence test) | **done, tested** |

| xterm.js 6 `src/common` (parser, InputHandler, Buffer, BufferReflow, unicode V6) | what is on a terminal's screen | `vt/*` | `fixtures/vt/*` (184 streams) | **done, tested** (2026-11-18) |
| `renderer/desk/bridge.js` + `lib/phone.js` frames | the wire: frame shapes, R8's "not a keystroke" test | `proto/frames.hpp` | `proto-synth.json` + inline | **done, tested** (2026-11-18) |

All four modules are implemented and pass `core/test/run.cpp` (**71/71**) under MinGW-w64 g++ 16.1.
`run.cpp` mirrors the JS unit tests; the JSON fixtures remain the language-neutral oracle (e.g. for a
future Swift port).

`triggers` has a header but no flat fixture file: a store's behaviour is a scripted SEQUENCE (set true,
stay true, go false, rise again), not one input→one output, so its oracle is the sequence in
`test/special-prompts-unit.js`. The port follows that, not a JSON vector.

## Notes per module, for the implementer

### layout (`model/layout.hpp`)
- Occupancy is a **sorted `std::vector<int>`** of reading-order cell numbers, always a filled rectangle.
  `LayoutMap` is a `std::map` (not unordered) so serialisation and `place()` iteration are deterministic —
  the JS relies on `Object.entries` insertion order being stable; a sorted map is the safe C++ equivalent.
- `place()` must not mutate its input (the JS returns a fresh object). Return by value.
- **Iteration order is load-bearing.** The JS object iterates in insertion order and that sets the order of
  `evicted`/`shrunk`. Port the layout as an insertion-ordered container (a `vector<pair>`), not a sorted
  `std::map`, or the displaced-tile lists come out in a different order than the fixtures. (The layout.json
  vectors were generated with already-sorted keys, so they will not flag this — the trap is in the header.)
- The R15 `directionalHalf` operates on the 2×2 quadrant cells `1,2,3,4` only; `dropTarget` routes to it
  solely when `dims.R==2 && dims.C==2`. Larger grids use `cellAt`. Do not generalise the triangle split.
- Watch the fraction maths: `rect` divides by `R`/`C`; keep it in `double` and compare fixtures with a
  tolerance of 0 — the JS produces exact IEEE doubles (e.g. `1/3`), so the port must too (same operations,
  same order) rather than rounding.

### hibernate (when ported)
- Three separable exports (test / triggers / decision) — keep them separate; the shells swap the DECISION
  for a real memory-pressure signal while the TEST stays. The `shownBy` set is `win:<index>` tokens; the
  policy keys on emptiness, never the token value (a pre-existing JS bug — see the F1 note in the brief —
  was a key() written for one type accepting another; do not reintroduce it).

### triggers (when ported)
- The rising-edge time (`at`) is what makes a gated prompt fire once per false→true edge. The inbox
  file-watch is shell I/O, not core — the core only holds the map and the edge logic.

### prompts / queue (when ported)
- `dueReason(item, now)` is the portable heart (the four kinds' gating). Delivery (bracketed paste, the
  confirm) is shell I/O. Port `dueReason`, `inWindow`, `minutesOfDay`; leave `deliver` to the shell.

## Build

See `core/README.md` §"Build & test". A compiler has been on the box since 2026-09-06.
