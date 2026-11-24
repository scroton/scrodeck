// core/deck/presence.cpp — see presence.hpp. lib/presence.js, function for function; the `deliver` sentences word for word.
#include "presence.hpp"
#include "util.hpp"
#include "../session/jsutil.hpp"
#include <algorithm>
#include <cmath>

namespace deck { namespace core {

namespace js = deck::session::js;
using ojson = nlohmann::ordered_json;

static bool isKind(const std::string& k) { return k == "office" || k == "remote" || k == "phone"; }

int awayMinutes(const json& v) {
  if (v.is_null() || (v.is_string() && v.get_ref<const std::string&>().empty())) return kDefaultAwayMinutes;
  const double n = v.is_object() || v.is_array() ? std::nan("") : js::toNumber(v);
  if (!std::isfinite(n)) return kDefaultAwayMinutes;
  return (int)std::max(0.0, std::min(1440.0, js::round(n)));
}
std::string deskKind(const json& v) { return v == "native" ? "native" : "browser"; }
json presenceName(const json& v) {
  if (!v.is_string()) return json();
  std::string s;
  for (unsigned char c : v.get_ref<const std::string&>()) if (!(c < 0x20 || c == 0x7f)) s += (char)c;
  s = js::sliceUnits(js::trim(s), 40);
  return s.empty() ? json() : json(s);
}
std::vector<std::pair<std::string, std::string>> tileEnv(const std::string& tileId, const std::string& stateFile) {
  return { { "CLAUDE_DECK_TILE", tileId }, { "CLAUDE_DECK_STATE", stateFile } };
}

Presence::Presence(const json& d, const json& awaySetting, double now, const json& prev, const std::string& pcName) : awayMin_(awayMinutes(awaySetting)), lastAt_(now), since_(now) {
  pcName_ = pcName;
  const json& sa = js::get(d, "startedAt");
  double started = now;
  if (sa.is_number() && std::isfinite(sa.get<double>())) started = sa.get<double>();
  else if (sa.is_string()) { const double p = js::dateParse(sa.get<std::string>()); if (!std::isnan(p) && p != 0) started = p; }
  deck_ = ojson::object();
  deck_["pid"] = ojson(js::get(d, "pid"));
  deck_["startedAt"] = isoTime(started);
  deck_["version"] = ojson(js::get(d, "version"));
  // A restart: the last input's screen carries over (the restart itself is the newest sign of life, so the away
  // clock starts now). `since` carries over too when the place did not change.
  if (prev.is_object() && js::get(prev, "v") == 1 && js::get(prev, "lastInputFrom").is_string() && isKind(js::get(prev, "lastInputFrom").get<std::string>())) {
    lastKind_ = js::get(prev, "lastInputFrom").get<std::string>();
    where_ = lastKind_;
    const double was = js::get(prev, "since").is_string() ? js::dateParse(js::get(prev, "since").get<std::string>()) : std::nan("");
    if (js::get(prev, "where") == where_ && std::isfinite(was) && was <= now) since_ = was;
  }
}

bool Presence::place(double now) {
  const bool away = awayMin_ > 0 && now - lastAt_ >= awayMin_ * 60000.0;
  const std::string where = away ? "away" : (lastKind_.empty() ? "office" : lastKind_);
  if (where == where_) return false;
  where_ = where; since_ = now;
  return true;
}
bool Presence::input(const std::string& kind, double now) {
  if (!isKind(kind)) return false;
  const bool moved = lastKind_ != kind;
  lastKind_ = kind; lastAt_ = now;
  return place(now) || moved;
}
bool Presence::connections(const json& remote, bool phone, double now) {
  const bool has = js::truthy(remote);
  const std::string kind = has ? deskKind(js::get(remote, "kind")) : std::string();
  const json name = has ? presenceName(js::get(remote, "name")) : json();
  const bool changed = has != hasRemote_ || kind != remoteKind_ || name != remoteName_ || phone != phone_;
  hasRemote_ = has; remoteKind_ = kind; remoteName_ = name; phone_ = phone;
  return place(now) || changed;
}
bool Presence::tick(double now) { return place(now); }
bool Presence::setAwayMinutes(const json& v, double now) {
  const int n = awayMinutes(v);
  if (n == awayMin_) return false;
  awayMin_ = n;
  place(now);
  return true;
}

static const char* const SEND = "send files with SendUserFile, or give a link";

ojson Presence::state() const {
  ojson s = ojson::object();
  s["v"] = 1;
  s["deck"] = deck_;
  s["where"] = where_;
  s["since"] = isoTime(since_);
  if (hasRemote_) { ojson r = ojson::object(); r["kind"] = remoteKind_; r["name"] = ojson(remoteName_); s["remote"] = r; } else s["remote"] = nullptr;
  { ojson p = ojson::object(); p["attached"] = phone_; s["phone"] = p; }
  s["lastInputFrom"] = lastKind_.empty() ? ojson() : ojson(lastKind_);
  const std::string desk = hasRemote_ ? (remoteName_.is_string() ? remoteName_.get<std::string>() : std::string("unnamed")) + ", " + (remoteKind_ == "native" ? "native app" : "browser") : std::string();
  std::string line;
  if (where_ == "office") line = "The user is at the office PC (the deck's own window): a local path, or opening the file on this PC, works.";
  else if (where_ == "remote") {
    // a remote desk running on the deck's OWN PC (its name is this PC's, any case): the user IS at this PC
    auto lower = [](std::string x) { for (char& ch : x) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a'); return x; };
    const bool herePc = hasRemote_ && remoteName_.is_string() && !remoteName_.get<std::string>().empty() && !pcName_.empty() && lower(remoteName_.get<std::string>()) == lower(pcName_);
    if (herePc) line = "The user is on a remote desk window on this PC (" + desk + "): a local path, or opening the file on this PC, works.";
    else line = hasRemote_
      ? "The user is on a remote desk (" + desk + "), not at this PC: they cannot open files on this PC. To hand something over, " + SEND + "."
      : std::string("The user was last active on a remote desk, which is not connected now: they are not at this PC. To hand something over, ") + SEND + ".";
  } else if (where_ == "phone") {
    line = phone_
      ? std::string("The user is on the phone, not at this PC: keep it short. To hand something over, ") + SEND + "."
      : std::string("The user was last active on the phone, which is not connected now: keep it short. To hand something over, ") + SEND + ".";
  } else {
    const std::string place = lastKind_ == "office" ? "the office PC" : lastKind_ == "remote" ? "a remote desk" : "the phone";
    const std::string last = lastKind_.empty() ? std::string("no input since the deck started") : "last input: " + place;
    std::string now;
    if (hasRemote_) now = "a remote desk (" + desk + ") is connected";
    if (phone_) now += std::string(now.empty() ? "" : ", ") + "the phone is connected";
    line = "No input on any screen for " + std::to_string(awayMin_) + "+ minutes (" + last + (now.empty() ? "" : "; " + now) + "): where the user will read this is unknown. Give the local path AND " + SEND + ".";
  }
  s["deliver"] = line;
  return s;
}
std::string Presence::text() const { return state().dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n"; }

}}  // namespace deck::core
