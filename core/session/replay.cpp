// core/session/replay.cpp — see replay.hpp. Ported from main.js; checked against core/test/fixtures/session/replay.json.
#include "replay.hpp"

namespace deck { namespace session {

size_t utf16Length(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) n++;          // one unit per character...
    if (c >= 0xF0) n++;                   // ...and a second for anything outside the BMP
  }
  return n;
}

std::string sliceFromUtf16(const std::string& s, size_t skip) {
  size_t i = 0;
  while (i < s.size() && skip > 0) {
    const unsigned char c = (unsigned char)s[i];
    size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (i + len > s.size()) len = s.size() - i;
    const size_t units = c >= 0xF0 ? 2 : 1;
    if (units > skip) return "\xEF\xBF\xBD" + s.substr(i + len);   // the cut split a surrogate pair
    skip -= units; i += len;
  }
  return s.substr(i);
}

// ---------------------------------------------------------------- Utf8Decoder (the WHATWG decoder, streaming)
void Utf8Decoder::reset() { held_.clear(); needed_ = 0; seen_ = 0; lower_ = 0x80; upper_ = 0xBF; }

std::string Utf8Decoder::write(const char* bytes, size_t n) {
  std::string out;
  out.reserve(n + held_.size());
  static const char kBad[] = "\xEF\xBF\xBD";
  for (size_t i = 0; i < n; i++) {
    const unsigned char b = (unsigned char)bytes[i];
    if (needed_ == 0) {
      if (b < 0x80) out += (char)b;
      else if (b >= 0xC2 && b <= 0xDF) { needed_ = 1; held_.assign(1, (char)b); }
      else if (b >= 0xE0 && b <= 0xEF) { if (b == 0xE0) lower_ = 0xA0; if (b == 0xED) upper_ = 0x9F; needed_ = 2; held_.assign(1, (char)b); }
      else if (b >= 0xF0 && b <= 0xF4) { if (b == 0xF0) lower_ = 0x90; if (b == 0xF4) upper_ = 0x8F; needed_ = 3; held_.assign(1, (char)b); }
      else out += kBad;
      continue;
    }
    if (b < lower_ || b > upper_) {
      held_.clear(); needed_ = 0; seen_ = 0; lower_ = 0x80; upper_ = 0xBF;
      out += kBad;
      i--;                                // this byte starts over as a first byte
      continue;
    }
    lower_ = 0x80; upper_ = 0xBF;
    held_ += (char)b;
    if (++seen_ == needed_) { out += held_; held_.clear(); needed_ = 0; seen_ = 0; }
  }
  return out;
}

std::string Utf8Decoder::end() {
  const bool pending = needed_ != 0;
  reset();
  return pending ? std::string("\xEF\xBF\xBD") : std::string();
}

// ---------------------------------------------------------------- Modes
static const int kModes[6] = { 1049, 1000, 1002, 1003, 1006, 2004 };
static int modeIndex(int mode) { for (int i = 0; i < 6; i++) if (kModes[i] == mode) return i; return -1; }

void Modes::reset() { for (bool& b : m_) b = false; }
bool Modes::on(int mode) const { const int i = modeIndex(mode); return i >= 0 && m_[i]; }

// const MODE_RE = /\x1b\[\?(1049|1000|1002|1003|1006|2004)([hl])/g  — one mode per sequence, nothing else
// between the '?' and the letter (a combined "?1000;1006h" does not match, as in main.js).
void Modes::track(const std::string& d) {
  size_t i = 0;
  while ((i = d.find("\x1b[?", i)) != std::string::npos) {
    const size_t p = i + 3;
    if (p + 5 <= d.size()) {
      int mode = 0; bool digits = true;
      for (size_t k = 0; k < 4; k++) { const char c = d[p + k]; if (c < '0' || c > '9') { digits = false; break; } mode = mode * 10 + (c - '0'); }
      const char set = d[p + 4];
      const int idx = digits ? modeIndex(mode) : -1;
      if (idx >= 0 && (set == 'h' || set == 'l')) { m_[idx] = set == 'h'; i = p + 5; continue; }
    }
    i += 1;                               // the regex resumes one character on from a failed start
  }
}

std::string Modes::prefix() const {
  std::string out;
  if (m_[0]) out += "\x1b[?1049h\x1b[2J\x1b[H";            // alternate screen, cleared
  for (int i = 1; i < 6; i++) if (m_[i]) out += "\x1b[?" + std::to_string(kModes[i]) + "h";
  return out;
}

// ---------------------------------------------------------------- Replay
void Replay::reset() { chunks_.clear(); bytes_ = total_ = dropped_ = cut_ = 0; }

void Replay::push(const std::string& d) {
  const size_t len = utf16Length(d);
  chunks_.push_back({ d, len });
  bytes_ += len; total_ += len;
  while (bytes_ > kReplayLimit && chunks_.size() > 1) {
    const size_t gone = chunks_.front().len;
    chunks_.pop_front();
    bytes_ -= gone; dropped_ += gone;
  }
}

void Replay::pushUncounted(const std::string& d) { chunks_.push_back({ d, utf16Length(d) }); }

void Replay::markResize() { cut_ = total_; }

std::string Replay::all() const {
  std::string out;
  for (const auto& c : chunks_) out += c.text;
  return out;
}

std::string Replay::sinceResize() const {
  if (chunks_.empty()) return std::string();
  if (cut_ <= dropped_) return all();                       // skip <= 0
  size_t skip = cut_ - dropped_;
  std::string out;
  for (const auto& c : chunks_) {
    if (skip >= c.len) { skip -= c.len; continue; }
    out += skip > 0 ? sliceFromUtf16(c.text, skip) : c.text;
    skip = 0;
  }
  return out;
}

std::string replayFor(const Replay& replay, const Modes& modes, bool sinceResize, bool* any) {
  const std::string pre = modes.prefix();
  const std::string body = sinceResize ? replay.sinceResize() : replay.all();
  if (any) *any = !pre.empty() || !body.empty();
  return pre + body;
}

}}  // namespace deck::session
