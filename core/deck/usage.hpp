// core/deck/usage.hpp — lib/usage.js UsagePoller, ported: per-account usage from the OAuth usage endpoint, at
// most once per account per 3 minutes (sooner when the credentials changed), with the disk cache and the ten
// minute back-off after a 429. PURE over three seams: Credentials (the token is read, never written, never
// kept), Http (one GET) and Files (the cache: usage numbers only). Tested with fakes only; no shell wires a
// real Credentials or Http to it (windows/HOST-NOTES.md: it waits for the owner's word).
#pragma once
#include "rules.hpp"
#include "seams.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace deck { namespace core {

extern const char* const kUsageUrl;                 // https://api.anthropic.com/api/oauth/usage
constexpr double kUsageRefreshMs = 180000;          // REFRESH_MS
constexpr double kUsageBackoff429Ms = 600000;       // BACKOFF_429_MS

class UsagePoller {
 public:
  UsagePoller(Clock& clock, Files& files, Http* http, Credentials* creds, std::string cacheFile);
  // Every account; `done` runs once each has answered (or was skipped).
  void pollAll(const std::vector<Account>& accounts, bool force, std::function<void()> done);
  const json& data() const { return data_; }        // account id -> { state, fetchedAt, plan, session, week, scoped, error }
  int requests() const { return requests_; }        // GETs made (tests)
 private:
  void pollOne(const Account& a, bool force, std::function<void()> done);
  void saveCache();
  Clock& clock_; Files& files_; Http* http_; Credentials* creds_;
  std::string cacheFile_;
  json data_ = json::object();
  std::map<std::string, std::string> stamps_;
  std::map<std::string, double> backoffUntil_;
  int requests_ = 0;
};

}}  // namespace deck::core
