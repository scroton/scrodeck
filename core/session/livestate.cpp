// core/session/livestate.cpp — see livestate.hpp.
#include "livestate.hpp"
#include "jsutil.hpp"

namespace deck { namespace session {

using json = nlohmann::json;
using namespace js;

static json orNull(const json& v) { return truthy(v) ? v : json(); }
static double numOr0(const json& v) { return v.is_number() ? v.get<double>() : 0; }

json LiveRecord::toJson() const {
  return { { "account", account }, { "status", status }, { "statusUpdatedAt", statusUpdatedAt }, { "alive", alive }, { "pid", pid },
    { "name", name }, { "bridgeSessionId", bridgeSessionId }, { "waitingFor", waitingFor },
    { "kind", kind }, { "cwd", cwd }, { "startedAt", startedAt }, { "procStart", procStart } };
}

bool LiveMap::add(const std::string& accountId, const std::string& fileText, const AliveFn& pidAlive) {
  // JSON.parse(readFileSync(f, 'utf8')): a BOM is not skipped (it throws), so such a file is no record
  if (startsWith(fileText, "\xEF\xBB\xBF")) return false;
  const json j = json::parse(fileText, nullptr, false);
  if (j.is_discarded() || !truthy(j)) return false;
  const json& sid = js::get(j, "sessionId");
  const json& pid = js::get(j, "pid");
  if (!truthy(sid) || !truthy(pid)) return false;
  if (!sid.is_string()) return false;                        // (a Map key of another type never matches a tile's id)
  LiveRecord cand;
  cand.account = accountId;
  cand.status = truthy(js::get(j, "status")) ? js::get(j, "status") : json("idle");
  cand.statusUpdatedAt = numOr0(orElse(js::get(j, "statusUpdatedAt"), js::get(j, "updatedAt")));
  // process.kill(pid, 0) takes anything that loosely equals a 32-bit integer ("1234" too) and throws a
  // TypeError for the rest, which reads as "not running"
  { const double n = (pid.is_number() || pid.is_string() || pid.is_boolean()) ? toNumber(pid) : std::nan("");
    const bool int32 = n == std::floor(n) && n >= -2147483648.0 && n <= 2147483647.0;
    cand.alive = int32 && pidAlive && pidAlive(n); }
  cand.pid = pid;
  cand.name = orNull(js::get(j, "name"));
  cand.bridgeSessionId = orNull(js::get(j, "bridgeSessionId"));
  cand.waitingFor = orNull(js::get(j, "waitingFor"));
  cand.kind = orNull(js::get(j, "kind"));
  cand.cwd = orNull(js::get(j, "cwd"));
  cand.startedAt = numOr0(js::get(j, "startedAt"));
  cand.procStart = orNull(js::get(j, "procStart"));
  const std::string& key = str(sid);
  all_.emplace_back(key, cand);
  for (auto& kv : items_) {
    if (kv.first != key) continue;
    const LiveRecord& prev = kv.second;
    if ((cand.alive && !prev.alive) || (cand.alive == prev.alive && cand.statusUpdatedAt > prev.statusUpdatedAt)) kv.second = std::move(cand);
    return true;
  }
  items_.emplace_back(key, std::move(cand));
  return true;
}

const LiveRecord* LiveMap::get(const std::string& sessionId) const {
  for (const auto& kv : items_) if (kv.first == sessionId) return &kv.second;
  return nullptr;
}

}}  // namespace deck::session
