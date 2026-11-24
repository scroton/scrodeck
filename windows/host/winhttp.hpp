// windows/host/winhttp.hpp — the two seams the usage poller (core/deck/usage.hpp) needs from Windows:
//   WinHttpGet       core::Http: one GET through WinHTTP on a worker thread, 15 s at most (lib/usage.js's
//                    AbortController); the answer is handed to `post`, which the shell runs on the deck's thread
//   WinCredentials   core::Credentials: lib/platform.js readOAuth / credentialsStamp on Windows —
//                    <configdir>\.credentials.json parsed, and its modification time as the change signal.
//                    READ ONLY: nothing here writes, refreshes or copies a token, and nothing logs one.
// NOT WIRED BY DEFAULT. LocalDeckOptions::usagePolling turns them on; no flag, config key or environment
// variable of the app sets it (windows/HOST-NOTES.md: reading the accounts' sign-in tokens waits for the owner).
#pragma once
#include "../../core/deck/seams.hpp"
#include <functional>
#include <memory>

namespace deck { namespace host {

class WinHttpGet : public core::Http {
 public:
  using Post = std::function<void(std::function<void()>)>;     // run this on the deck's thread
  explicit WinHttpGet(Post post);
  ~WinHttpGet() override;                                       // joins every request still in flight
  void get(const std::string& url, const std::map<std::string, std::string>& headers, std::function<void(const core::HttpResponse&)> done) override;
 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

class WinCredentials : public core::Credentials {
 public:
  core::json readOAuth(const std::string& configDir) override;
  std::string stamp(const std::string& configDir) override;
};

}}  // namespace deck::host
