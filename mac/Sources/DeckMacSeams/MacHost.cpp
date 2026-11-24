// mac/Sources/DeckMacSeams/MacHost.cpp — dms_host_create: every macOS seam, wired to one inbox, as a DBHost.
// UNBUILT, UNTESTED: written on Windows, never compiled.
// Windows counterpart: the seam members and constructor of windows/host/localdeck.cpp (LocalDeck::Impl) /
// windows/src/deckhost.hpp (DeckHost::create). JS mirrored: main.js's requires (node-pty, fs, http, lib/platform.js).
#include "MacSeams.hpp"
#include "DeckMacSeams.h"

#include <atomic>
#include <signal.h>

namespace {
struct MacHost : DBHost {
  deck::mac::MacClock clockS;
  deck::mac::PosixFiles filesS;
  deck::mac::MacRandom randomS;
  deck::mac::MacProcessList procsS;
  std::unique_ptr<deck::mac::MacEnv> envS;
  std::unique_ptr<deck::mac::MacPtyHost> ptysS;
  std::unique_ptr<deck::mac::MacListener> listenerS;
  std::unique_ptr<deck::mac::MacNet> netS;
  std::unique_ptr<deck::core::Http> httpS;
  std::unique_ptr<deck::core::Credentials> credentialsS;

  explicit MacHost(const DMSOptions& o) {
    envS.reset(new deck::mac::MacEnv(o.login_environment != 0));
    ptysS.reset(new deck::mac::MacPtyHost(inbox));
    clock = &clockS; files = &filesS; random = &randomS; procs = &procsS; env = envS.get(); ptys = ptysS.get();
    if (o.server) {
      listenerS.reset(new deck::mac::MacListener(inbox, o.test_port_low, o.test_port_high));
      netS.reset(new deck::mac::MacNet(o.test_port_low > 0));
      listener = listenerS.get(); net = netS.get();
    }
    if (o.http_and_credentials) {
      httpS = deck::mac::makeUrlSessionHttp(inbox);
      credentialsS = deck::mac::makeKeychainCredentials(clockS);
      http = httpS.get(); credentials = credentialsS.get();
    }
  }
  // db_deck_shutdown: the listener was stopped by the bridge (its thread joined); a request in flight is dropped.
  // The pty reader threads end by themselves when their children do (Deck::shutdown killed them).
  void quiesce() override { httpS.reset(); http = nullptr; }
};
}  // namespace

DBHost* dms_host_create(const DMSOptions* options) {
  // a pty or a socket whose other end went away must be an EPIPE, not the end of the deck
  static std::atomic<bool> once{ false };
  if (!once.exchange(true)) signal(SIGPIPE, SIG_IGN);
  DMSOptions o{}; if (options) o = *options;
  return new MacHost(o);
}
