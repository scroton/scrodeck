// core/deck/driverrule.cpp — see driverrule.hpp. lib/driverrule.js, function by function.
#include "driverrule.hpp"

namespace deck { namespace core { namespace driverrule {

const char* const MODES[4] = { "auto", "office", "phone", "remote" };
const char* const KINDS[3] = { "office", "phone", "remote" };

std::string mode(const json& v) {
  if (!v.is_string()) return std::string();
  const std::string& s = v.get_ref<const std::string&>();
  if (s == "desk") return "office";
  for (const char* m : MODES) if (s == m) return s;
  return std::string();
}
static std::string modeOrAuto(const json& m) { const std::string md = mode(m); return md.empty() ? std::string("auto") : md; }

std::string driverOf(const json& m, const Who& w) {
  const std::string md = modeOrAuto(m);
  if (md == "office") return "office";
  if (md == "phone") return "phone";
  if (md == "remote") return w.remote ? "remote" : "office";
  return w.phone ? "phone" : w.remote ? "remote" : "office";
}

bool acceptsSize(const json& m, const Who& who, const std::string& from) { return driverOf(m, who) == from; }

static json sizeOrNull(const json& ptySize) {       // `ptySize || null`
  if (ptySize.is_null() || (ptySize.is_boolean() && !ptySize.get<bool>())) return json();
  return ptySize;
}
json officeMirror(const json& m, const Who& who, const json& ptySize) { return driverOf(m, who) == "office" ? json() : sizeOrNull(ptySize); }
json remoteMirror(const json& m, const Who& who, const json& ptySize) {
  if (!who.remote) return json();
  return driverOf(m, who) == "remote" ? json() : sizeOrNull(ptySize);
}

bool phoneMirrors(const json& m) { const std::string md = modeOrAuto(m); return md == "office" || md == "remote"; }

bool inputLocked(const json& m, const Who& who, const std::string& from) { return from == "office" && driverOf(m, who) != "office"; }

std::string refusedReason(const json& m, const Who& who) {
  const std::string d = driverOf(m, who);
  if (d == "office") return std::string();
  const bool claimed = modeOrAuto(m) != "auto";
  return d == "phone" ? (claimed ? "phone" : "phone-attached") : (claimed ? "remote" : "remote-showing");
}

std::string click(const json& m, const std::string& me) { return modeOrAuto(m) == me ? std::string("auto") : me; }
std::string look(const json& m, const std::string& me) { const std::string md = modeOrAuto(m); return md == "auto" ? "auto" : md == me ? "mine" : "other"; }

}}}  // namespace deck::core::driverrule
