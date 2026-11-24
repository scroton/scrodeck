// core/test/deck/cloud_test.cpp — core/deck/cloud.{hpp,cpp} against core/test/fixtures/deck/cloud.json, which
// core/test/deck/gen-cloud.js wrote by running the real lib/cloud.js (and main.js cloudSpec) on synthetic input.
// The poller is the real C++ class on fake::Http / fake::Creds / fake::Clock: nothing here reads a credential or
// opens a socket. Requests are answered one at a time, later (answerAtOnce = false), as a shell's pump would.
#include "t.hpp"
#include "fakes.hpp"
#include "../../deck/cloud.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

void testCloud(const std::string& fixtures);

using namespace deck::core;

namespace {

// JSON.parse's own wording for a body that is not JSON is not reproduced: both sides read "Unexpected"
json soft(json v) {
  if (v.is_object() && v.contains("error") && v["error"].is_string() && v["error"].get<std::string>().compare(0, 10, "Unexpected") == 0) v["error"] = "Unexpected";
  return v;
}
std::string grow(const json& r) {
  std::string out = str(r["pre"]);
  const std::string unit = str(r["unit"]);
  for (int i = 0, n = r["times"].get<int>(); i < n; i++) out += unit;
  return out + str(r["post"]);
}
std::string shortText(const std::string& s) { return s.size() > 60 ? s.substr(0, 60) + "..." : s; }

void pure(const json& f) {
  const json& k = f["constants"];
  check("cloud: TILE_MS", kCloudTileMs == k["TILE_MS"].get<double>());
  check("cloud: LIST_MS", kCloudListMs == k["LIST_MS"].get<double>());
  check("cloud: BACKOFF_MS", kCloudBackoffMs == k["BACKOFF_MS"].get<double>());
  check("cloud: CLOUD_TICK_MS", kCloudTickMs == k["CLOUD_TICK_MS"].get<double>());
  check("cloud: CLOUD_TASK_MAX", kCloudTaskMax == k["CLOUD_TASK_MAX"].get<int>());

  for (const json& c : f["ids"]) {
    const std::string in = str(c["in"]), name = "cloud id " + c["in"].dump();
    check(name + ": cloudKey", cloudKey(in) == str(c["key"]), cloudKey(in));
    check(name + ": webId", c["webId"].is_null() ? cloudWebId(in).empty() : cloudWebId(in) == str(c["webId"]), cloudWebId(in));
    check(name + ": webUrl", cloudWebUrl(in) == str(c["webUrl"]), cloudWebUrl(in));
    check(name + ": isCloudId", isCloudId(in) == c["isCloudId"].get<bool>());
  }
  int i = 0;
  for (const json& c : f["created"]) {
    const json got = cloudParseCreated(str(c["in"]));
    check("cloud parseCreated #" + std::to_string(i++) + " " + shortText(c["out"].dump()), got == c["out"], got.dump());
  }
  i = 0;
  for (const json& c : f["views"]) {
    const json got = cloudView(c["in"]);
    check("cloud view #" + std::to_string(i++) + " " + shortText(c["in"].dump()), got == c["out"], got.dump() + "\n      want " + c["out"].dump());
  }
  i = 0;
  for (const json& c : f["listable"]) {
    check("cloud listable #" + std::to_string(i++) + " " + shortText(c["in"].dump()), cloudListable(c["in"]) == c["out"].get<bool>());
  }
  i = 0;
  for (const json& c : f["tasks"]) {
    const std::string name = "cloud task #" + std::to_string(i++) + " " + shortText(c["in"].dump());
    const std::string clean = cloudCleanTask(c["in"]), error = cloudTaskError(clean);
    if (!c["task"].is_null()) check(name + ": cleaned", clean == str(c["task"]), clean);
    check(name + ": what cloudSpec says", error == str(c["error"]), error);
  }
  i = 0;
  for (const json& c : f["longTasks"]) {
    const std::string name = "cloud long task #" + std::to_string(i++) + " " + shortText(c["in"].dump());
    const std::string clean = cloudCleanTask(json(grow(c["in"]))), error = cloudTaskError(clean);
    if (!c["task"].is_null()) check(name + ": cleaned", clean == grow(c["task"]), shortText(clean));
    check(name + ": what cloudSpec says", error == str(c["error"]), error);
  }
}

std::vector<CloudAccount> accountsOf(const json& list) {
  std::vector<CloudAccount> out;
  for (const json& a : list) out.push_back({ str(a["id"]), str(a["dir"]) });
  return out;
}
std::vector<CloudTile> tilesOf(const json& list) {
  std::vector<CloudTile> out;
  for (const json& t : list) out.push_back({ str(t["cloudId"]), str(t["account"]) });
  return out;
}

void scenario(const json& sc) {
  fake::Clock clock; fake::Http http; fake::Creds creds;
  http.answerAtOnce = false;
  CloudPoller poller(clock, &http, &creds);
  poller.setAccounts(accountsOf(sc["accounts"]));
  for (const json& s : sc["steps"]) {
    const std::string name = "cloud [" + str(sc["name"]) + "] " + str(s["label"]);
    clock.t = s["clock"].get<double>();
    const double advance = s["advance"].get<double>();
    if (!s["setAccounts"].is_null()) poller.setAccounts(accountsOf(s["setAccounts"]));
    creds.oauth.clear();
    for (auto it = s["creds"].begin(); it != s["creds"].end(); ++it) creds.oauth[it.key()] = it.value();
    for (const json& id : s["soon"]) poller.soon(str(id));
    const std::vector<CloudTile> tiles = tilesOf(s["tiles"]);

    const size_t before = http.asked.size();
    static const json none;
    bool done = false, changed = false;
    poller.poll(tiles, [&](bool c) { done = true; changed = c; });
    // (the JS pass is busy until its promise settles; here a pass that makes no request has ended inside poll())
    const bool out = !s["calls"].empty();
    check(name + ": busy exactly while a request is out", poller.busy() == out && done == !out && (!out || s["busyDuring"].get<bool>()));
    if (s["concurrent"].get<bool>()) {
      bool done2 = false, changed2 = true;
      poller.poll(tiles, [&](bool c) { done2 = true; changed2 = c; });
      check(name + ": a second pass answers at once", done2 && changed2 == s["second"].get<bool>());
    }
    // answer what was asked, one request after another; each answer lets the pass make its next request
    for (size_t i = before; i < http.asked.size(); i++) {
      check(name + ": one request at a time", i + 1 == http.asked.size() && !done);
      clock.t += advance;
      const auto h = http.asked[i].headers.find("Authorization");
      const std::string token = h == http.asked[i].headers.end() || h->second.size() < 7 ? std::string() : h->second.substr(7);
      HttpResponse r;
      const json& forToken = s["responses"].contains(token) ? s["responses"][token] : none;
      if (forToken.is_object() && forToken.contains(http.asked[i].url)) {
        const json& c = forToken[http.asked[i].url];
        r.status = c["status"].get<int>();
        if (c.contains("headers")) for (auto it = c["headers"].begin(); it != c["headers"].end(); ++it) r.headers[it.key()] = str(it.value());
        r.body = c.contains("text") ? str(c["text"]) : c.contains("json") ? c["json"].dump() : std::string();
      } else r.error = "fetch failed";
      const std::function<void(const HttpResponse&)> answer = http.asked[i].done;   // (asked grows under the call)
      answer(r);
    }
    check(name + ": the pass ended", done && poller.busy() == s["busyAfter"].get<bool>());
    check(name + ": changed", changed == s["changed"].get<bool>());
    check(name + ": the clock ends where the JS's did", clock.t == s["clockAfter"].get<double>());
    const json& calls = s["calls"];
    check(name + ": how many requests", http.asked.size() - before == calls.size(), std::to_string(http.asked.size() - before));
    for (size_t i = 0; i < calls.size() && before + i < http.asked.size(); i++) {
      const auto& a = http.asked[before + i];
      const json& c = calls[i];
      check(name + ": request " + std::to_string(i) + " url", a.url == str(c["url"]), a.url);
      std::vector<std::string> names;
      for (const auto& kv : a.headers) names.push_back(kv.first);
      std::sort(names.begin(), names.end());
      const bool shape = json(names) == c["headers"] && a.headers.count("anthropic-beta") && a.headers.at("anthropic-beta") == str(c["beta"])
        && a.headers.count("anthropic-version") && a.headers.at("anthropic-version") == str(c["version"]);
      check(name + ": request " + std::to_string(i) + " headers (names, beta, version)", shape, json(names).dump());
      // the bearer is the token of the account the JS asked as (made-up tokens; never printed)
      std::string dir;
      for (const json& acc : s["accounts"]) if (acc["id"] == c["account"]) { dir = str(acc["dir"]); break; }
      const json want = s["creds"].contains(dir) && s["creds"][dir].is_object() && s["creds"][dir].contains("claudeAiOauth") ? s["creds"][dir]["claudeAiOauth"].value("accessToken", json()) : json();
      check(name + ": request " + std::to_string(i) + " is asked as " + str(c["account"]), want.is_string() && a.headers.count("Authorization") && a.headers.at("Authorization") == "Bearer " + str(want));
    }
    for (auto it = s["sessions"].begin(); it != s["sessions"].end(); ++it) {
      const json got = soft(poller.session(it.key()));
      check(name + ": session(" + json(it.key()).dump() + ")", got == soft(it.value()), got.dump() + "\n      want " + it.value().dump());
    }
    check(name + ": lists", poller.lists() == s["lists"], poller.lists().dump());
    for (const json& e : s["elsewhere"]) {
      std::set<std::string> hosted;
      for (const json& k : e["hosted"]) hosted.insert(str(k));
      const json got = poller.elsewhere(hosted);
      check(name + ": elsewhere(" + e["hosted"].dump() + ")", got == e["out"], got.dump());   // arrays: the order is compared too
    }
  }
}

void seams() {
  fake::Clock clock;
  const std::vector<CloudAccount> accounts = { { "a1", "D:/fake/a1" } };
  const std::vector<CloudTile> tiles = { { "session_NoSeams00001", "a1" } };
  { // no seams at all: every tile reads as not logged in, nothing is asked, the pass still ends
    CloudPoller p(clock, nullptr, nullptr); p.setAccounts(accounts);
    bool done = false, changed = false;
    p.poll(tiles, [&](bool c) { done = true; changed = c; });
    check("cloud: no seams, the pass ends", done && changed && !p.busy());
    check("cloud: no seams, the tile says not logged in", p.session("cse_NoSeams00001").value("error", "") == "not logged in, or the login has expired", p.session("cse_NoSeams00001").dump());
    check("cloud: no seams, elsewhere is empty", p.elsewhere({}) == json::array() && p.lists() == json::object());
  }
  { // credentials but no Http: the same, and the credentials are not even read
    fake::Creds creds; creds.oauth["D:/fake/a1"] = { { "claudeAiOauth", { { "accessToken", "made-up" } } } };
    CloudPoller p(clock, nullptr, &creds); p.setAccounts(accounts);
    bool done = false;
    p.poll(tiles, [&](bool) { done = true; });
    check("cloud: no Http seam, nothing is read or asked", done && creds.reads == 0 && p.session("session_NoSeams00001").value("error", "") == "not logged in, or the login has expired");
  }
  { // Http but no credentials seam
    fake::Http http;
    CloudPoller p(clock, &http, nullptr); p.setAccounts(accounts);
    bool done = false;
    p.poll(tiles, [&](bool) { done = true; });
    check("cloud: no credentials seam, no request", done && http.asked.empty());
  }
  { // no done callback: a pass still runs and ends
    fake::Http http; fake::Creds creds; creds.oauth["D:/fake/a1"] = { { "claudeAiOauth", { { "accessToken", "made-up" } } } };
    CloudPoller p(clock, &http, &creds); p.setAccounts(accounts);
    p.poll(tiles, nullptr);
    check("cloud: no done callback, the pass ends (network errors answered at once)", !p.busy() && http.asked.size() == 2 && p.session("session_NoSeams00001").value("error", "") == "fetch failed");
    p.poll(tiles, nullptr); p.poll({}, nullptr);
    check("cloud: ... and nothing is due straight after", http.asked.size() == 2);
  }
  { // an answer that arrives after the poller is gone is dropped
    fake::Http http; http.answerAtOnce = false;
    fake::Creds creds; creds.oauth["D:/fake/a1"] = { { "claudeAiOauth", { { "accessToken", "made-up" } } } };
    bool done = false;
    {
      CloudPoller p(clock, &http, &creds); p.setAccounts(accounts);
      p.poll(tiles, [&](bool) { done = true; });
      check("cloud: a request is out", p.busy() && http.asked.size() == 1);
    }
    http.answer(0);
    check("cloud: a late answer to a poller that is gone does nothing", !done && http.asked.size() == 1);
  }
  { // done may start the next pass itself
    fake::Http http; fake::Creds creds;
    CloudPoller p(clock, &http, &creds); p.setAccounts(accounts);
    int passes = 0;
    p.poll(tiles, [&](bool) { passes++; p.poll(tiles, [&](bool c) { if (!c) passes++; }); });
    check("cloud: done can poll again", passes == 2 && !p.busy());
  }
}

}  // namespace

void testCloud(const std::string& fixtures) {
  if (!want("cloud")) return;
  const json f = loadFixture(fixtures, "cloud");
  pure(f);
  for (const json& sc : f["scenarios"]) scenario(sc);
  seams();
}
