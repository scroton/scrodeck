// core/model/triggers.hpp — the trigger store, ported from lib/triggers.js (R17). The STORE and its
// rising-edge rule are pure and belong in the core; the INBOX (a watched directory a session drops files
// into) is shell I/O and stays in the shell — the shell parses a drop and calls set() here.
//
// STATUS: header only (no compiler on the box). Behaviour is covered by test/special-prompts-unit.js;
// there is no flat fixture file because a store's behaviour is a scripted SEQUENCE (set true, set true
// again, set false, set true) rather than one input->one output. Port against that unit test's sequence.
//
// The one rule that matters and is easy to get wrong: `set(name, true)` records the moment of a
// false->true transition as the trigger's edge time `at`; staying true does NOT move it; going false
// clears it. A triggered prompt fires once per edge by comparing its own firedAt against risenAt().
#ifndef CLAUDE_DECK_CORE_TRIGGERS_HPP
#define CLAUDE_DECK_CORE_TRIGGERS_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace deck::triggers {

struct Trigger { std::string name; bool value = false; int64_t at = 0; std::string by; };

// name slug: letters, digits, . _ - ; 1..64 chars. Returns empty for an invalid name.
std::string cleanName(const std::string& name);

class Store {
 public:
  // set a value; on a false->true transition records `now` as the edge time. by = who set it.
  const Trigger* set(const std::string& name, bool value, const std::string& by, int64_t now);
  const Trigger* declare(const std::string& name, const std::string& by);   // ensure it exists (false), no edge
  bool remove(const std::string& name);
  bool value(const std::string& name) const;
  int64_t risenAt(const std::string& name) const;     // edge time if currently true, else 0
  const Trigger* get(const std::string& name) const;
  std::vector<Trigger> list() const;                  // sorted by name

  // serialisation: the shell persists this (triggers.json) and reloads it. Keep the map here; the shell
  // owns the file. (toJson/fromJson left to the shell's JSON lib.)
  const std::map<std::string, Trigger>& map() const { return map_; }

 private:
  std::map<std::string, Trigger> map_;
};

}  // namespace deck::triggers

#endif  // CLAUDE_DECK_CORE_TRIGGERS_HPP
