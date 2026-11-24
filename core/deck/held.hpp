// core/deck/held.hpp — special prompts (lib/queue.js HeldPrompts), ported: a message the deck keeps for one
// session and types in when a condition is met — timed, next (the next idle-at-prompt), recurring (a daily
// window + interval), triggered (a named trigger's rising edge). PURE: the tile, the clock, the file and the
// timers come in as functions. The due rule is the one core/model/prompts.hpp also holds; this file is the whole
// class (the list, its file, the delivery: bracketed paste, Enter, the busy confirmation).
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <functional>
#include <string>
#include <vector>

namespace deck { namespace core {

using json = nlohmann::json;

// lib/queue.js promptVisible: the prompt marker (❯ or >) at the start of a line of the recent output
bool promptVisible(const std::string& recent);
bool heldInWindow(int mins, int start, int end);

struct HeldTile { bool exists = false; std::string kind; json sessionId; bool exited = false, hasPty = false; std::string status; };

struct HeldDeps {
  std::function<double()> now;
  std::function<int(double)> minutesOfDay;                       // local minutes since midnight at an instant
  std::function<std::string()> uuid;
  std::function<bool(std::string&)> load;                         // the file's text; false when there is none
  std::function<void(const std::string&)> save;
  std::function<HeldTile(const std::string& tileId)> tile;        // tileById + statusOf
  std::function<std::string(const std::string& tileId)> recentOutput;
  std::function<void(const std::string& tileId, const std::string& data)> write;
  std::function<void(const std::string& tileId)> noteInput;
  std::function<double(const std::string& name)> triggerRisenAt;
  std::function<void()> onChange;
  std::function<void(double ms, std::function<void()>)> later;    // setTimeout
  double confirmMs = 3000;                                        // 0 = deliver + confirm at once (tests)
};

class HeldPrompts {
 public:
  explicit HeldPrompts(HeldDeps deps);
  json add(const std::string& tileId, const json& spec);          // throws std::runtime_error with the JS's words
  bool remove(const std::string& id);
  void forgetTile(const std::string& tileId);
  json forTile(const std::string& tileId) const;                  // [pub(item)], in display order
  json list() const;
  void tick();
  size_t countFor(const std::string& tileId) const;
 private:
  json pub(const json& i) const;
  double sortKey(const json& i) const;
  bool due(const json& i, double now) const;
  void changed();
  void deliver(const std::string& itemId, double now);
  bool confirmedNow(const std::string& tileId) const;
  void finishSent(const std::string& id, double now, bool confirmed);
  void confirm(const std::string& id, const std::string& tileId, double now, int tries);
  json* find(const std::string& id);
  HeldDeps d_;
  std::vector<json> items_;
};

}}  // namespace deck::core
