// core/deck/usage.cpp — see usage.hpp. lib/usage.js, line for line.
#include "usage.hpp"
#include "util.hpp"
#include "../session/jsutil.hpp"
#include <cmath>
#include <memory>

namespace deck { namespace core {

namespace js = deck::session::js;
const char* const kUsageUrl = "https://api.anthropic.com/api/oauth/usage";

static json numOrNull(const json& v) { return v.is_number() && std::isfinite(v.get<double>()) ? v : json(); }
static json stamp(double d) { return d == std::floor(d) ? json((int64_t)d) : json(d); }

UsagePoller::UsagePoller(Clock& clock, Files& files, Http* http, Credentials* creds, std::string cacheFile)
  : clock_(clock), files_(files), http_(http), creds_(creds), cacheFile_(std::move(cacheFile)) {
  std::string text;
  if (!cacheFile_.empty() && files_.read(cacheFile_, text)) {
    const json cached = json::parse(text, nullptr, false);
    if (cached.is_object()) for (auto it = cached.begin(); it != cached.end(); ++it) {
      if (!js::truthy(js::get(it.value(), "session")) || !js::truthy(js::get(it.value(), "week"))) continue;
      json v = it.value(); v["state"] = "cached"; v["fetchedAt"] = 0;
      data_[it.key()] = v;
    }
  }
}

void UsagePoller::saveCache() {
  if (cacheFile_.empty()) return;
  json keep = json::object();
  for (auto it = data_.begin(); it != data_.end(); ++it) {
    const json& v = it.value();
    if (!js::truthy(js::get(v, "session")) || !js::truthy(js::get(v, "week"))) continue;
    keep[it.key()] = { { "plan", js::get(v, "plan") }, { "session", js::get(v, "session") }, { "week", js::get(v, "week") },
      { "scoped", js::truthy(js::get(v, "scoped")) ? js::get(v, "scoped") : json::array() }, { "savedAt", stamp(clock_.nowMs()) } };
  }
  files_.makeDirs(dirName(cacheFile_));
  files_.write(cacheFile_, keep.dump(-1, ' ', false, json::error_handler_t::replace));
}

void UsagePoller::pollAll(const std::vector<Account>& accounts, bool force, std::function<void()> done) {
  if (accounts.empty()) { if (done) done(); return; }
  std::shared_ptr<int> left = std::make_shared<int>((int)accounts.size());
  for (const Account& a : accounts) pollOne(a, force, [left, done] { if (--*left == 0 && done) done(); });
}

void UsagePoller::pollOne(const Account& a, bool force, std::function<void()> done) {
  const std::string id = a.id;
  const json cur = data_.contains(id) ? data_[id] : json::object();
  auto set = [this, id, cur](const char* state, const json& more) {
    json v = cur; v["state"] = state; v["fetchedAt"] = stamp(clock_.nowMs());
    if (more.is_object()) for (auto it = more.begin(); it != more.end(); ++it) v[it.key()] = it.value();
    data_[id] = v;
  };
  if (!creds_ || !http_) { set("no-credentials", json()); done(); return; }
  const std::string st = creds_->stamp(a.dir);
  if (st.empty()) { set("no-credentials", json()); done(); return; }
  const bool changed = stamps_.find(id) == stamps_.end() || stamps_[id] != st;
  stamps_[id] = st;
  const double now = clock_.nowMs();
  const double fetchedAt = js::get(cur, "fetchedAt").is_number() ? js::get(cur, "fetchedAt").get<double>() : 0;
  if (!force && !changed && fetchedAt != 0 && now - fetchedAt < kUsageRefreshMs) { done(); return; }
  if (backoffUntil_[id] > now) { done(); return; }

  const json parsed = creds_->readOAuth(a.dir);
  const json& cred = js::get(parsed, "claudeAiOauth");
  if (!js::truthy(cred) || !js::truthy(js::get(cred, "accessToken"))) { set("no-credentials", json()); done(); return; }
  const json plan = js::truthy(js::get(cred, "subscriptionType")) ? js::get(cred, "subscriptionType") : (js::truthy(js::get(cur, "plan")) ? js::get(cur, "plan") : json());
  const json& exp = js::get(cred, "expiresAt");
  if (js::truthy(exp) && js::toNumber(exp) < now) { set("expired", { { "plan", plan } }); done(); return; }

  std::map<std::string, std::string> headers = {
    { "Authorization", "Bearer " + js::toString(js::get(cred, "accessToken")) },
    { "anthropic-beta", "oauth-2025-04-20" }, { "Content-Type", "application/json" } };
  requests_++;
  http_->get(kUsageUrl, headers, [this, id, cur, plan, set, done](const HttpResponse& res) {
    if (!res.error.empty()) { set("error", { { "plan", plan }, { "error", res.error } }); done(); return; }
    if (res.status == 401) { set("expired", { { "plan", plan } }); done(); return; }
    if (res.status == 429) {
      double retry = 0;
      for (const auto& h : res.headers) if (lowerAscii(h.first) == "retry-after") { const double v = js::toNumber(json(h.second)); retry = std::isnan(v) ? 0 : v * 1000; }
      backoffUntil_[id] = clock_.nowMs() + (retry > 0 ? retry : kUsageBackoff429Ms);
      set("rate-limited", { { "plan", plan } }); done(); return;
    }
    if (res.status < 200 || res.status > 299) { set("error", { { "plan", plan }, { "error", "HTTP " + std::to_string(res.status) } }); done(); return; }
    const json j = json::parse(res.body, nullptr, false);
    if (j.is_discarded()) { set("error", { { "plan", plan }, { "error", "Unexpected response (not JSON)" } }); done(); return; }
    json scoped = json::array();
    const json& limits = js::get(j, "limits");
    if (limits.is_array()) for (const json& l : limits) {
      const json& model = js::get(js::get(l, "scope"), "model");
      if (!js::truthy(l) || js::get(l, "kind") != "weekly_scoped" || !js::truthy(model)) continue;
      const json& dn = js::get(model, "display_name");
      scoped.push_back({ { "model", js::truthy(dn) ? dn : (js::truthy(js::get(model, "id")) ? js::get(model, "id") : json("")) },
        { "used", numOrNull(js::get(l, "percent")) }, { "resetsAt", js::truthy(js::get(l, "resets_at")) ? js::get(l, "resets_at") : json() } });
    }
    auto window = [&](const char* k) {
      const json& w = js::get(j, k);
      return json{ { "used", numOrNull(js::get(w, "utilization")) }, { "resetsAt", js::truthy(js::get(w, "resets_at")) ? js::get(w, "resets_at") : json() } };
    };
    data_[id] = { { "state", "ok" }, { "fetchedAt", stamp(clock_.nowMs()) }, { "plan", plan }, { "session", window("five_hour") }, { "week", window("seven_day") }, { "scoped", scoped } };
    saveCache();
    done();
  });
}

}}  // namespace deck::core
