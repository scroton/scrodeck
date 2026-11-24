// core/session/replay.hpp — a tile's replay window and screen-mode memory, ported from main.js
// (spawnTile's onData, trackModes, modePrefix, replaySinceResize, resizeTile / nudgeTile's `replayCut`,
// and the `replayFor` the phone server is given). PURE: no OS calls.
//
// What main.js keeps per tile, and so what this keeps:
//   replay[]        the pty's output chunks, newest last, exactly as node-pty delivered them
//   replayBytes     the sum of their lengths                       (REPLAY_LIMIT = 512 * 1024)
//   replayTotal     every length ever pushed
//   replayDropped   the lengths trimmed off the front
//   replayCut       replayTotal at the moment the pty was last resized
// The trim drops WHOLE chunks from the front while the window is over the limit and more than one chunk is
// left; nothing is ever cut inside a chunk, and nothing looks for an escape sequence: a window can start in
// the middle of one (the mode prefix replayed ahead of it is what keeps a fresh terminal in the right buffer).
// `sinceResize` is the one place a chunk is cut: at the exact offset the resize happened.
//
// LENGTHS ARE JAVASCRIPT STRING LENGTHS. node-pty hands main.js UTF-8-decoded strings, so every count above
// is in UTF-16 code units, not bytes: "é" counts 1, an emoji 2. This port stores UTF-8 and counts the same
// way (utf16Length), so the limit trims at the same chunk and `sinceResize` cuts at the same character as
// the Electron deck. The one case UTF-8 cannot follow: a cut that lands between the two halves of a
// surrogate pair. JavaScript keeps the lone low half; here that character becomes U+FFFD.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace deck { namespace session {

constexpr size_t kReplayLimit = 512 * 1024;

// JavaScript's `s.length` for a (well-formed) UTF-8 string.
size_t utf16Length(const std::string& utf8);
// `s.slice(skip)` with `skip` in UTF-16 units: the UTF-8 of what is left.
std::string sliceFromUtf16(const std::string& utf8, size_t skip);

// Node's StringDecoder('utf8'), which is what stands between the pty and main.js (node-pty calls
// socket.setEncoding('utf8')): bytes in, whole characters out. A sequence split across two reads is held
// until its rest arrives; ill-formed input becomes U+FFFD (one per maximal invalid subpart).
class Utf8Decoder {
 public:
  std::string write(const char* bytes, size_t n);
  std::string end();                      // what an unfinished sequence becomes when the stream ends
  void reset();
 private:
  std::string held_;                      // the bytes of the sequence in progress
  int needed_ = 0, seen_ = 0;
  unsigned char lower_ = 0x80, upper_ = 0xBF;
};

// main.js MODE_RE: the six private modes a fresh view must be told about before it sees the window.
class Modes {
 public:
  void reset();                           // spawnTile: `tile.modes = {}`
  void track(const std::string& chunk);   // trackModes(tile, d): one chunk at a time; a sequence split
                                          // across two chunks is not seen (as in main.js)
  std::string prefix() const;             // modePrefix(tile)
  bool on(int mode) const;                // 1049, 1000, 1002, 1003, 1006, 2004
  bool altScreen() const { return on(1049); }
  bool mouseOn() const { return on(1000) || on(1002) || on(1003); }
  bool sgrMouse() const { return on(1006); }
 private:
  bool m_[6] = { false, false, false, false, false, false };
};

class Replay {
 public:
  void reset();                           // spawnTile: replay [], bytes / total / dropped / cut 0
  void push(const std::string& chunk);    // onData: push, count, trim to the limit
  void pushUncounted(const std::string& chunk);   // `tile.replay.push(...)` alone: the "[claude-deck] failed to start" line
  void markResize();                      // resizeTile / nudgeTile: replayCut = replayTotal
  std::string all() const;                // replay.join('')
  std::string sinceResize() const;        // replaySinceResize(t)
  bool empty() const { return chunks_.empty(); }
  size_t chunks() const { return chunks_.size(); }
  size_t bytes() const { return bytes_; }       // replayBytes   (UTF-16 units, see the top)
  size_t total() const { return total_; }       // replayTotal
  size_t dropped() const { return dropped_; }   // replayDropped
  size_t cut() const { return cut_; }           // replayCut
 private:
  struct Chunk { std::string text; size_t len; };
  std::deque<Chunk> chunks_;
  size_t bytes_ = 0, total_ = 0, dropped_ = 0, cut_ = 0;
};

// What an attaching view is sent: modePrefix + (sinceResize ? replaySinceResize : replay.join('')).
// `any` is false when both halves are empty — main.js then sends nothing at all (the phone's replayFor
// answers null, tile:attach skips the send).
std::string replayFor(const Replay& replay, const Modes& modes, bool sinceResize, bool* any = nullptr);

}}  // namespace deck::session
