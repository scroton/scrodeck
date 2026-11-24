// core/test/deck/t.hpp — what the two halves of the deck test share.
#pragma once
#include "../../third_party/nlohmann/json.hpp"
#include "../../deck/edition.hpp"
#include "../../deck/hash.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using json = nlohmann::json;

extern int passed, failed;
extern std::string only, lastCheck;
inline void check(const std::string& name, bool ok, const std::string& got = "") {
  lastCheck = name;
  if (ok) { ++passed; return; }
  ++failed;
  std::printf("FAIL  %s%s%s\n", name.c_str(), got.empty() ? "" : "\n      ", got.substr(0, 900).c_str());
  std::fflush(stdout);
}
inline bool want(const char* section) { return only.empty() || only == section; }
inline json loadFixture(const std::string& dir, const char* name) {
  std::ifstream f(dir + "/" + name + ".json", std::ios::binary);
  if (!f) { std::printf("cannot open fixture %s/%s.json\n", dir.c_str(), name); std::exit(2); }
  std::stringstream ss; ss << f.rdbuf();
  return json::parse(ss.str());
}
inline std::string unhex(const std::string& h) {
  std::string out;
  auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
  for (size_t i = 0; i + 1 < h.size(); i += 2) out += (char)(v(h[i]) * 16 + v(h[i + 1]));
  return out;
}
inline std::string str(const json& v) { return v.is_string() ? v.get<std::string>() : std::string(); }
// a recorded route body's { length, sha256 } as THIS edition serves it: the fixtures are the Electron deck's, whose
// token page says "Claude Deck: this link needs its token"; the public build names itself there (core/deck/edition.hpp)
inline std::pair<size_t, std::string> editionBody(const json& res) {
  std::pair<size_t, std::string> out{ res["length"].get<size_t>(), str(res["sha256"]) };
  const std::string from = "Claude Deck: this link needs its token", to = std::string(deck::edition::kProductName) + ": this link needs its token";
  std::string t = str(res.contains("text") ? res["text"] : json());
  if (from == to || t.find(from) == std::string::npos) return out;
  for (size_t at = t.find(from); at != std::string::npos; at = t.find(from, at + to.size())) t.replace(at, from.size(), to);
  return { t.size(), deck::core::hexOf(deck::core::sha256(t)) };
}

void testScenarios(const std::string& fixtures, const std::string& repoRoot);   // deck_scenarios.cpp
void testServer(const std::string& fixtures, const std::string& repoRoot);      // deck_server.cpp
void testAsks(const std::string& fixtures);                                     // asks_test.cpp      (lib/asks.js)
void testViewer(const std::string& fixtures);                                   // viewer_test.cpp    (lib/viewer.js)
void testDeskOpen(const std::string& fixtures);                                 // deskopen_test.cpp  (lib/deskopen.js)
void testCloud(const std::string& fixtures);                                    // cloud_test.cpp     (lib/cloud.js)
void testRemoteRules(const std::string& fixtures);                              // remoterule_test.cpp: lib/remoterule.js, lib/driverrule.js, and the deck under them
void testParity(const std::string& fixtures);                                   // deck_parity_test.cpp: those four as the deck uses them
void testAskCards(const std::string& fixtures);                                 // askcards_test.cpp: core/deck/askcards (the native ask cards: page.html's logic)
int askCardsEmit(const std::string& page, const std::string& steps, const std::string& out);   // askcards_test.cpp: deck-test <fx> <repo> askcards-emit ...
void testHostingAndOptions(const std::string& fixtures);                        // hosting_test.cpp: core/deck/options + core/deck/hosting (sections options, hosting)
void testOneDeck(const std::string& fixtures);                                  // onedeck_test.cpp: core/deck/onedeck (lib/onedeck.js), section onedeck
