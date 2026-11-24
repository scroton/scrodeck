// core/deck/remote.cpp — see remote.hpp.
#include "remote.hpp"
#include "driverrule.hpp"
#include "../session/jsutil.hpp"
#include "../session/replay.hpp"       // utf16Length
#include <algorithm>
#include <cmath>

namespace deck { namespace core {

namespace js = deck::session::js;

std::string clipMessage(const std::string& text, int lines) {
  if (text.empty()) return text;
  const size_t cap = (size_t)lines * kMsgCharsPerLine;
  size_t end = text.size(), from = 0; int seen = 0;
  while (from <= text.size()) {
    const size_t nl = text.find('\n', from);
    const size_t stop = nl == std::string::npos ? text.size() : nl;
    if (!js::trim(text.substr(from, stop - from)).empty() && ++seen == lines) { end = stop; break; }
    if (nl == std::string::npos) break;
    from = nl + 1;
  }
  const std::string head = text.substr(0, end);
  return deck::session::utf16Length(head) > cap ? js::sliceUnits(text, cap) : head;
}

static double numOr(const json& v, double dflt) {      // +v || dflt
  const double d = js::toNumber(v);
  return (std::isnan(d) || d == 0) ? dflt : d;
}

json remoteMeta(const json& meta, bool desk) {
  if (!meta.is_object() || !js::get(meta, "tiles").is_array()) return meta;
  const json& ml = js::get(js::get(meta, "settings"), "msgLines");
  const double normal = numOr(js::get(ml, "normal"), 3), wide = numOr(js::get(ml, "wide"), 10);
  const int lines = (int)std::max(1.0, desk ? std::max(normal, wide) : normal);
  json out = meta;
  for (json& t : out["tiles"]) {
    if (!t.is_object()) continue;
    auto it = t.find("lastText");
    if (it == t.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) continue;
    *it = clipMessage(it->get_ref<const std::string&>(), lines);
  }
  return out;
}

json deskMeta(const json& meta, const std::set<std::string>& shown, bool hidden, const std::function<bool(const std::string&)>& phoneWatching) {
  if (!meta.is_object() || !js::get(meta, "tiles").is_array()) return meta;
  const json& d = js::get(js::get(meta, "settings"), "driver");
  const std::string mode = js::truthy(d) ? js::toString(d) : "auto";
  json out = meta;
  for (json& t : out["tiles"]) {
    if (!t.is_object()) continue;
    const std::string id = js::str(js::get(t, "id"));
    const bool shows = !hidden && shown.count(id) > 0;
    const bool drives = shows && mode != "desk" && !(phoneWatching && phoneWatching(id));
    const json& size = js::get(t, "ptySize");
    t["mirror"] = drives ? json() : (shows && js::truthy(size) ? size : json());
  }
  return out;
}

json deskMeta(const json& meta, const std::set<std::string>& shown, bool hidden, const std::function<bool(const std::string&)>& phoneWatching, const DeskRule& rule) {
  if (!rule.claim) {
    json out = deskMeta(meta, shown, hidden, phoneWatching);
    if (rule.ownWindowsOnly && out.is_object() && js::get(out, "tiles").is_array()) for (json& t : out["tiles"]) if (t.is_object()) t["inWindow"] = nullptr;
    return out;
  }
  if (!meta.is_object() || !js::get(meta, "tiles").is_array()) return meta;
  const json& d = js::get(js::get(meta, "settings"), "driver");
  const json mode = js::truthy(d) ? d : json("auto");
  json out = meta;
  for (json& t : out["tiles"]) {
    if (!t.is_object()) continue;
    const std::string id = js::str(js::get(t, "id"));
    driverrule::Who who; who.remote = !hidden && shown.count(id) > 0; who.phone = phoneWatching && phoneWatching(id);
    const json& size = js::get(t, "ptySize");
    t["mirror"] = driverrule::remoteMirror(mode, who, js::truthy(size) ? size : json());
    if (rule.ownWindowsOnly) t["inWindow"] = nullptr;
  }
  return out;
}

std::string metaKey(const json& meta, const json& assets, const json& build) {
  json m = meta;
  if (m.is_object()) m["now"] = 0;
  return json::array({ assets, build, m }).dump(-1, ' ', false, json::error_handler_t::replace);
}

}}  // namespace deck::core
