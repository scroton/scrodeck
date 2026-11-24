// core/model/triggers.cpp — the trigger store + rising-edge rule, ported from lib/triggers.js. The inbox
// (a watched directory) is shell I/O and lives in the shell. Oracle: the scripted sequence in
// test/special-prompts-unit.js.
#include "triggers.hpp"

namespace deck::triggers {

// NAME_RE = /^[A-Za-z0-9_.-]{1,64}$/
std::string cleanName(const std::string& name) {
  // trim
  size_t a = name.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = name.find_last_not_of(" \t\r\n");
  std::string n = name.substr(a, b - a + 1);
  if (n.empty() || n.size() > 64) return "";
  for (char c : n) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok) return "";
  }
  return n;
}

const Trigger* Store::set(const std::string& name, bool value, const std::string& by, int64_t now) {
  std::string n = cleanName(name);
  if (n.empty()) return nullptr;
  auto it = map_.find(n);
  bool prevValue = it != map_.end() ? it->second.value : false;
  int64_t prevAt = it != map_.end() ? it->second.at : 0;
  std::string prevBy = it != map_.end() ? it->second.by : "";
  bool rising = value && !prevValue;
  Trigger t;
  t.name = n; t.value = value; t.at = rising ? now : prevAt; t.by = by.empty() ? prevBy : by;
  map_[n] = t;
  return &map_[n];
}

const Trigger* Store::declare(const std::string& name, const std::string& by) {
  std::string n = cleanName(name);
  if (n.empty()) return nullptr;
  if (map_.find(n) == map_.end()) { Trigger t; t.name = n; t.value = false; t.at = 0; t.by = by; map_[n] = t; }
  return &map_[n];
}

bool Store::remove(const std::string& name) {
  std::string n = cleanName(name);
  if (n.empty()) return false;
  return map_.erase(n) > 0;
}

const Trigger* Store::get(const std::string& name) const {
  std::string n = cleanName(name);
  if (n.empty()) return nullptr;
  auto it = map_.find(n);
  return it == map_.end() ? nullptr : &it->second;
}

bool Store::value(const std::string& name) const { const Trigger* t = get(name); return t && t->value; }
int64_t Store::risenAt(const std::string& name) const { const Trigger* t = get(name); return (t && t->value) ? t->at : 0; }

std::vector<Trigger> Store::list() const {
  std::vector<Trigger> out;
  for (auto& kv : map_) out.push_back(kv.second);   // std::map iterates sorted by name
  return out;
}

}  // namespace deck::triggers
