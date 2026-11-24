// core/deck/remoterule.cpp — see remoterule.hpp. lib/remoterule.js, function by function.
#include "remoterule.hpp"
#include <algorithm>

namespace deck { namespace core { namespace remoterule {

static bool has(const std::vector<std::string>& v, const std::string& id) { return std::find(v.begin(), v.end(), id) != v.end(); }

int holder(const std::string& id, const std::vector<Entry>& entries) {
  for (size_t i = 0; i < entries.size(); i++) if (!entries[i].remote && has(entries[i].layout, id)) return (int)i;
  return -1;
}

std::vector<Eviction> evictions(size_t taker, bool takerRemote, const std::vector<std::string>& layout, const std::vector<Entry>& entries, bool oneWindow) {
  std::vector<Eviction> out;
  if (!oneWindow || takerRemote) return out;
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].remote || i == taker) continue;
    Eviction e; e.entry = i;
    for (const std::string& id : entries[i].layout) if (has(layout, id)) e.ids.push_back(id);
    if (!e.ids.empty()) out.push_back(std::move(e));
  }
  return out;
}

std::vector<std::string> officeShownBy(const std::string& id, const std::vector<Entry>& entries) {
  std::vector<std::string> out;
  for (const Entry& w : entries) if (!w.remote && w.visible && has(w.shown, id)) out.push_back("win:" + std::to_string(w.index));
  return out;
}
bool officeShows(const std::string& id, const std::vector<Entry>& entries) { return !officeShownBy(id, entries).empty(); }

std::vector<std::string> shownBy(const std::string& id, const std::vector<Entry>& entries, bool phoneWatching) {
  std::vector<std::string> out = officeShownBy(id, entries);
  if (phoneWatching) out.push_back("remote");
  return out;
}

std::string answerer(bool officeShows, bool remoteShows, bool phoneWatching) {
  if (officeShows) return "office";
  if (remoteShows) return "remote";
  if (phoneWatching) return "phone";
  return "office";
}

std::vector<std::string> wakesForRemote(const Shown& prev, const Shown& next, const std::function<bool(const std::string&)>& asleep) {
  std::vector<std::string> out;
  if (!next.has || next.hidden) return out;
  static const std::vector<std::string> none;
  const std::vector<std::string>& before = (prev.has && !prev.hidden) ? prev.shown : none;
  for (const std::string& id : next.shown) if (!has(before, id) && asleep(id)) out.push_back(id);
  return out;
}

}}}  // namespace deck::core::remoterule
