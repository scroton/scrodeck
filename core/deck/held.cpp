// core/deck/held.cpp — see held.hpp. lib/queue.js, function for function.
#include "held.hpp"
#include "../session/jsutil.hpp"
#include "../session/replay.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace deck { namespace core {

namespace js = deck::session::js;

static const double HOLD_WINDOW = 6 * 3600 * 1000.0;
static const double KEEP_DONE = 24 * 3600 * 1000.0;
static const size_t TEXT_MAX = 20000;
static const int DAY = 24 * 60;

static json numJ(double d) { return d == std::floor(d) && std::fabs(d) < 9e15 ? json((int64_t)d) : json(d); }
static double num(const json& v) { return v.is_number() ? v.get<double>() : 0; }

// stripAnsi: CSI sequences become a line break, OSC ... BEL goes
static std::string stripAnsi(const std::string& s) {
  std::string out; out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      size_t j = i + 2;
      while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';' || s[j] == '?' || s[j] == '<' || s[j] == '=' || s[j] == '>')) j++;
      if (j < s.size() && ((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z'))) { out += '\n'; i = j + 1; continue; }
    } else if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == ']') {
      const size_t j = s.find('\x07', i + 2);
      if (j != std::string::npos) { i = j + 1; continue; }
    }
    out += s[i++];
  }
  return out;
}
bool promptVisible(const std::string& recentIn) {
  // .slice(-262144) counts UTF-16 units; a byte cut that lands inside a character only loses that character
  std::string recent = recentIn.size() > 262144 * 3 ? recentIn.substr(recentIn.size() - 262144 * 3) : recentIn;
  { const size_t units = deck::session::utf16Length(recent); if (units > 262144) recent = deck::session::sliceFromUtf16(recent, units - 262144); }
  const std::string s = stripAnsi(recent);
  auto markerAt = [&](size_t i) {                           // [ \t]*[❯>][ \t ]
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < s.size() && s[i] == '>') i++;
    else if (s.compare(i, 3, "\xE2\x9D\xAF") == 0) i += 3;
    else return false;
    return i < s.size() && (s[i] == ' ' || s[i] == '\t' || s.compare(i, 2, "\xC2\xA0") == 0);
  };
  if (markerAt(0)) return true;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\n') { if (markerAt(i + 1)) return true; }
    else if (s.compare(i, 3, "\xE2\x94\x80") == 0) { if (markerAt(i + 3)) return true; }
  }
  return false;
}
bool heldInWindow(int mins, int start, int end) {
  if (start == end) return true;
  return start < end ? (mins >= start && mins < end) : (mins >= start || mins < end);
}

HeldPrompts::HeldPrompts(HeldDeps deps) : d_(std::move(deps)) {
  std::string text;
  if (d_.load && d_.load(text)) {
    const json j = json::parse(text, nullptr, false);
    const json& items = js::get(j, "items");
    if (items.is_array()) for (const json& i : items) {
      if (!i.is_object() || !js::truthy(js::get(i, "id")) || !js::truthy(js::get(i, "tileId")) || !js::get(i, "text").is_string()) continue;
      json it = i;
      if (!js::truthy(js::get(it, "kind"))) it["kind"] = "timed";            // older files: timed prompts with an `at`
      if (js::get(it, "state") == "sending") it["state"] = "held";
      items_.push_back(it);
    }
  }
}

json* HeldPrompts::find(const std::string& id) { for (json& i : items_) if (js::get(i, "id") == id) return &i; return nullptr; }
void HeldPrompts::changed() {
  if (d_.save) d_.save(json{ { "items", items_ } }.dump(2, ' ', false, json::error_handler_t::replace));
  if (d_.onChange) d_.onChange();
}
static bool oneShot(const json& i) { const std::string k = js::str(js::get(i, "kind")); return k == "timed" || k == "next"; }
double HeldPrompts::sortKey(const json& i) const { return oneShot(i) ? num(js::get(i, "at")) : num(js::get(i, "createdAt")); }

json HeldPrompts::pub(const json& i) const {
  auto orNull = [&](const char* k) { const json& v = js::get(i, k); return js::truthy(v) ? v : json(); };
  auto keep = [&](const char* k) { return js::get(i, k); };                   // ?? null
  return { { "id", js::get(i, "id") }, { "tileId", js::get(i, "tileId") }, { "kind", js::get(i, "kind") }, { "text", js::get(i, "text") },
    { "createdAt", js::get(i, "createdAt") }, { "state", js::get(i, "state") }, { "at", orNull("at") }, { "everyMin", orNull("everyMin") },
    { "startMin", keep("startMin") }, { "endMin", keep("endMin") }, { "trigger", orNull("trigger") }, { "sentAt", orNull("sentAt") },
    { "lastSentAt", orNull("lastSentAt") }, { "note", orNull("note") } };
}

json HeldPrompts::add(const std::string& tileId, const json& spec) {
  const HeldTile t = d_.tile(tileId);
  if (!t.exists) throw std::runtime_error("no such session");
  if (t.kind != "claude") throw std::runtime_error("only a Claude session takes a special prompt");
  const json& rawText = js::get(spec, "text");
  std::string text = rawText.is_null() ? std::string() : js::toString(rawText);
  { std::string n; for (size_t k = 0; k < text.size(); k++) { if (text[k] == '\r') { n += '\n'; if (k + 1 < text.size() && text[k + 1] == '\n') k++; } else n += text[k]; } text = js::trim(n); }
  if (text.empty()) throw std::runtime_error("nothing to send");
  const size_t len = deck::session::utf16Length(text);
  if (len > TEXT_MAX) throw std::runtime_error("too long (" + std::to_string(len) + " characters; " + std::to_string(TEXT_MAX) + " at most)");
  const std::string kind = js::truthy(js::get(spec, "kind")) ? js::toString(js::get(spec, "kind")) : "timed";
  const double now = d_.now();
  json base = { { "id", d_.uuid() }, { "tileId", tileId }, { "sessionId", js::truthy(t.sessionId) ? t.sessionId : json() }, { "kind", kind }, { "text", text },
    { "createdAt", numJ(now) }, { "state", "held" }, { "tries", 0 }, { "note", nullptr } };
  auto n = [&](const char* k) { return js::toNumber(spec.is_object() && spec.contains(k) ? spec[k] : json(std::nan(""))); };
  if (kind == "timed") {
    const double when = n("at");
    if (!(when > 0)) throw std::runtime_error("no time given");
    base["at"] = numJ(when);
  } else if (kind == "next") {
    base["at"] = numJ(now);
  } else if (kind == "recurring") {
    const double every = js::round(n("everyMin"));
    if (!(every >= 1)) throw std::runtime_error("no interval given (minutes, at least 1)");
    auto wrap = [&](const char* k) { double v = n(k); if (std::isnan(v)) v = 0; const long long r = (long long)js::round(v); return (int)(((r % DAY) + DAY) % DAY); };
    base["everyMin"] = numJ(every); base["startMin"] = wrap("startMin"); base["endMin"] = wrap("endMin"); base["lastSentAt"] = 0;
  } else if (kind == "triggered") {
    const std::string trig = js::trim(js::truthy(js::get(spec, "trigger")) ? js::toString(js::get(spec, "trigger")) : std::string());
    if (trig.empty()) throw std::runtime_error("no trigger named");
    base["trigger"] = trig; base["firedAt"] = 0;
  } else throw std::runtime_error("unknown special-prompt kind: " + kind);
  items_.push_back(base);
  changed();
  return pub(base);
}
bool HeldPrompts::remove(const std::string& id) {
  const size_t before = items_.size();
  items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const json& i) { return js::get(i, "id") == id; }), items_.end());
  if (items_.size() != before) changed();
  return items_.size() != before;
}
void HeldPrompts::forgetTile(const std::string& tileId) {
  const size_t before = items_.size();
  items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const json& i) { return js::get(i, "tileId") == tileId; }), items_.end());
  if (items_.size() != before) changed();
}
size_t HeldPrompts::countFor(const std::string& tileId) const { size_t n = 0; for (const json& i : items_) if (js::get(i, "tileId") == tileId) n++; return n; }
json HeldPrompts::forTile(const std::string& tileId) const {
  std::vector<const json*> v;
  for (const json& i : items_) if (js::get(i, "tileId") == tileId) v.push_back(&i);
  std::stable_sort(v.begin(), v.end(), [&](const json* a, const json* b) { return sortKey(*a) < sortKey(*b); });
  json out = json::array(); for (const json* i : v) out.push_back(pub(*i));
  return out;
}
json HeldPrompts::list() const {
  std::vector<const json*> v;
  for (const json& i : items_) v.push_back(&i);
  std::stable_sort(v.begin(), v.end(), [&](const json* a, const json* b) { return sortKey(*a) < sortKey(*b); });
  json out = json::array(); for (const json* i : v) out.push_back(pub(*i));
  return out;
}

bool HeldPrompts::due(const json& i, double now) const {
  const std::string kind = js::str(js::get(i, "kind"));
  if (kind == "recurring") {
    if (!heldInWindow(d_.minutesOfDay ? d_.minutesOfDay(now) : 0, (int)num(js::get(i, "startMin")), (int)num(js::get(i, "endMin")))) return false;
    const double last = num(js::get(i, "lastSentAt"));
    if (last != 0 && now - last < num(js::get(i, "everyMin")) * 60000) return false;
    return true;
  }
  if (kind == "triggered") {
    const double risen = d_.triggerRisenAt ? d_.triggerRisenAt(js::str(js::get(i, "trigger"))) : 0;
    if (!(risen != 0)) return false;
    return !(num(js::get(i, "firedAt")) >= risen);
  }
  return num(js::get(i, "at")) <= now;
}

void HeldPrompts::tick() {
  const double now = d_.now();
  bool dirty = false;
  std::set<std::string> taken;
  for (const json& i : items_) if (js::get(i, "state") == "sending") taken.insert(js::str(js::get(i, "tileId")));
  std::vector<std::string> order;
  { std::vector<const json*> v; for (const json& i : items_) v.push_back(&i);
    std::stable_sort(v.begin(), v.end(), [&](const json* a, const json* b) { return sortKey(*a) < sortKey(*b); });
    for (const json* i : v) order.push_back(js::str(js::get(*i, "id"))); }
  auto hold = [&](json& i, const std::string& note) {
    if (js::get(i, "note") != note || js::get(i, "state") != "due") { i["state"] = "due"; i["note"] = note; if (!js::truthy(js::get(i, "heldSince"))) i["heldSince"] = numJ(now); }
  };
  auto fail = [&](json& i, const std::string& note) { i["state"] = "failed"; i["note"] = note; i["doneAt"] = numJ(now); };
  for (const std::string& id : order) {
    json* ip = find(id); if (!ip) continue;
    json& i = *ip;
    const std::string state = js::str(js::get(i, "state"));
    if (state == "sent" || state == "failed") {
      if (now - num(js::get(i, "doneAt")) > KEEP_DONE) { items_.erase(items_.begin() + (ip - &items_[0])); dirty = true; }
      continue;
    }
    if (state == "sending") continue;
    if (!due(i, now)) continue;
    const std::string tileId = js::str(js::get(i, "tileId"));
    if (taken.count(tileId)) { hold(i, "another special prompt is going in first"); dirty = true; continue; }
    const HeldTile t = d_.tile(tileId);
    if (!t.exists) { fail(i, "the session is no longer in the deck"); dirty = true; continue; }
    const double at = num(js::get(i, "at"));
    if (t.exited || !t.hasPty) {
      hold(i, "the session is not running");
      if (oneShot(i) && now - at > HOLD_WINDOW) fail(i, "the session was not running");
      dirty = true; continue;
    }
    std::string reason;
    if (t.status == "busy") reason = "the session is busy";
    else if (t.status == "waiting") reason = "the session is waiting for you (a question is up)";
    else if (t.status == "starting") reason = "the session is still starting";
    else if (t.status == "asleep") reason = "the session is asleep";
    else if (t.status != "idle") reason = "the session is " + t.status;
    else if (!promptVisible(d_.recentOutput(tileId))) reason = "no prompt on the screen yet";
    if (!reason.empty()) {
      if (oneShot(i) && now - at > HOLD_WINDOW) fail(i, "not delivered: " + reason + " for 6 hours after its time");
      else hold(i, reason);
      dirty = true; continue;
    }
    deliver(id, now);
    taken.insert(tileId);
    dirty = true;
  }
  if (dirty) changed();
}

void HeldPrompts::deliver(const std::string& id, double now) {
  json* ip = find(id); if (!ip) return;
  json& i = *ip;
  const std::string tileId = js::str(js::get(i, "tileId"));
  i["state"] = "sending"; i["tries"] = (int)num(js::get(i, "tries")) + 1; i["note"] = nullptr;
  d_.write(tileId, "\x1b[200~" + js::str(js::get(i, "text")) + "\x1b[201~");
  if (d_.noteInput) d_.noteInput(tileId);
  auto enter = [this, tileId] { const HeldTile t = d_.tile(tileId); if (t.exists && t.hasPty) d_.write(tileId, "\r"); };
  if (d_.confirmMs == 0) { enter(); finishSent(id, now, confirmedNow(tileId)); changed(); return; }
  d_.later(150, enter);
  d_.later(d_.confirmMs, [this, id, tileId, now] { confirm(id, tileId, now, 0); });
}
bool HeldPrompts::confirmedNow(const std::string& tileId) const {
  const HeldTile t = d_.tile(tileId);
  const std::string st = t.exists && t.hasPty ? t.status : "off";
  return st == "busy" || st == "waiting";
}
// a standing kind re-arms on the tick's clock; a one-shot is done
void HeldPrompts::finishSent(const std::string& id, double now, bool confirmed) {
  json* p = find(id); if (!p) return;
  json& it = *p;
  const std::string kind = js::str(js::get(it, "kind"));
  if (kind == "recurring") { it["lastSentAt"] = numJ(now); it["state"] = "held"; it["note"] = nullptr; }
  else if (kind == "triggered") { it["firedAt"] = numJ(d_.triggerRisenAt ? d_.triggerRisenAt(js::str(js::get(it, "trigger"))) : now); it["state"] = "held"; it["note"] = nullptr; }
  else { it["state"] = "sent"; it["sentAt"] = numJ(now); it["doneAt"] = numJ(now); it["note"] = nullptr; }
  if (!confirmed && oneShot(it)) it["note"] = "typed, but the session did not turn busy \xE2\x80\x94 check it";
}
void HeldPrompts::confirm(const std::string& id, const std::string& tileId, double now, int tries) {
  if (confirmedNow(tileId)) { finishSent(id, now, true); changed(); return; }
  const HeldTile t = d_.tile(tileId);
  if (tries < 2 && t.exists && t.hasPty) { d_.write(tileId, "\r"); d_.later(d_.confirmMs, [this, id, tileId, now, tries] { confirm(id, tileId, now, tries + 1); }); return; }
  finishSent(id, now, false); changed();
}

}}  // namespace deck::core
