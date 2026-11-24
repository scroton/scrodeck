// core/test/deck/deskopen_test.cpp — core/deck/deskopen.cpp against what the REAL lib/deskopen.js answered:
// core/test/fixtures/deck/deskopen.json, written by core/test/deck/gen-deskopen.js. The pure rules case by case,
// and the inbox as the same scripted runs on fake::MemFiles and fake::Clock, compared file by file after each drain.
#include "t.hpp"
#include "fakes.hpp"
#include "../../deck/deskopen.hpp"
#include <cmath>
#include <stdexcept>

void testDeskOpen(const std::string& fixtures);

namespace {
using namespace deck::core;
std::string brief(const std::string& s) { return s.size() > 90 ? s.substr(0, 90) + "..." : s; }
}  // namespace

void testDeskOpen(const std::string& fixtures) {
  if (!want("deskopen")) return;
  const json f = loadFixture(fixtures, "deskopen");

  check("deskopen DIR", std::string(kDeskOpenDir) == str(f["consts"]["DIR"]), kDeskOpenDir);
  check("deskopen PROMPT_MAX", kDeskPromptMax == f["consts"]["PROMPT_MAX"].get<size_t>());
  check("deskopen the poll and the prune", kDeskOpenPollMs == 1500 && kDeskPruneMs == 86400000.0);

  // ---- ID_RE
  for (const json& c : f["ids"]) check("deskopen ID_RE " + c["id"].dump(), deskOpenIdOk(str(c["id"])) == c["ok"].get<bool>());

  // ---- pickSpot
  { int i = 0;
    for (const json& c : f["spots"]) {
      const json got = deskPickSpot(c["windows"], c["grid"]);
      check("deskopen pickSpot #" + std::to_string(i++) + " grid " + c["grid"].dump() + " " + brief(c["windows"].dump()), got == c["out"], got.dump() + "  want " + c["out"].dump());
    } }

  // ---- checkRequest
  { int i = 0;
    for (const json& c : f["requests"]) {
      const json got = deskCheckRequest(c["raw"], str(c["id"]), c["now"].get<double>());
      const std::string label = "deskopen checkRequest #" + std::to_string(i++);
      check(label, got == c["out"], brief(got.dump()) + "\n      want " + brief(c["out"].dump()) + "\n      raw " + brief(c["raw"].dump()));
      if (got != c["out"] && got.is_object() && c["out"].is_object() && got.contains("req") && c["out"].contains("req"))
        for (auto it = c["out"]["req"].begin(); it != c["out"]["req"].end(); ++it)
          if (got["req"].value(it.key(), json("<missing>")) != it.value()) std::printf("      .%s: %s  want %s\n", it.key().c_str(), brief(got["req"].value(it.key(), json("<missing>")).dump()).c_str(), brief(it.value().dump()).c_str());
    } }

  // ---- accountFor
  { int i = 0;
    for (const json& c : f["acctCases"]) {
      json got = deskAccountFor(c["accounts"], str(c["dir"]), true);
      check("deskopen accountFor win32 #" + std::to_string(i) + " " + c["dir"].dump(), got == c["win"], got.dump() + "  want " + c["win"].dump());
      got = deskAccountFor(c["accounts"], str(c["dir"]), false);
      check("deskopen accountFor posix #" + std::to_string(i++) + " " + c["dir"].dump(), got == c["posix"], got.dump() + "  want " + c["posix"].dump());
    }
    check("deskopen accountFor: win32 is the default", deskAccountFor(f["acctCases"][0]["accounts"], str(f["acctCases"][0]["dir"])) == f["acctCases"][0]["win"]); }

  // ---- openPlan
  { int i = 0;
    for (const json& c : f["plans"]) {
      const json& how = c["isDir"];
      const std::function<bool(const std::string&)> isDir = [&](const std::string& d) {
        if (how.is_string()) return str(how) == "all";
        for (const json& x : how) if (str(x) == d) return true;
        return false;
      };
      json got = deskOpenPlan(c["req"], c["tiles"], c["accounts"], isDir, true);
      check("deskopen openPlan win32 #" + std::to_string(i), got == c["win"], brief(got.dump()) + "\n      want " + brief(c["win"].dump()));
      got = deskOpenPlan(c["req"], c["tiles"], c["accounts"], isDir, false);
      check("deskopen openPlan posix #" + std::to_string(i++), got == c["posix"], brief(got.dump()) + "\n      want " + brief(c["posix"].dump()));
    }
    const json& c = f["plans"][0];
    check("deskopen openPlan: no isDir given means every folder is there (isDir = () => true)", deskOpenPlan(c["req"], c["tiles"], c["accounts"], nullptr) == c["win"]); }

  // ---- placedWords
  for (const json& c : f["words"]) { const std::string got = deskPlacedWords(c["p"]); check("deskopen placedWords " + c["p"].dump(), got == str(c["out"]), got + "  want " + str(c["out"])); }

  // ---- the inbox: the same scripted runs
  for (const json& run : f["inbox"]) {
    const std::string name = "deskopen inbox [" + str(run["label"]) + "] ";
    fake::Clock clock; clock.t = run["now0"].get<double>();
    fake::MemFiles files; files.clock = &clock;
    const std::string dir = str(run["sub"]).empty() ? "C:\\userData\\desk-open" : "C:\\userData\\" + str(run["sub"]);
    json answer = { { "kind", "value" }, { "value", { { "ok", true }, { "sessionId", "0f3c2a9e-1b2c-4d5e-8f90-123456789abc" }, { "tileId", "tile-1" }, { "pid", 4242 }, { "window", 0 }, { "primary", true }, { "cells", { 6 } }, { "newWindow", false }, { "how", "opened" } } } };
    json opened = json::array();
    DeskOpenInbox* self = nullptr;
    DeskOpenInbox box(files, clock, dir, [&](const json& req) -> json {
      opened.push_back(req);
      const std::string kind = str(answer["kind"]);
      if (kind == "throw") throw std::runtime_error(str(answer["message"]));
      if (kind == "reenter") return { { "ok", true }, { "how", "opened" }, { "pid", self->drain().size() + 100 } };
      return answer["value"];
    });
    self = &box;
    check(name + "dir()", box.dir() == dir);
    int i = 0;
    for (const json& s : run["steps"]) {
      const std::string op = str(s["op"]);
      const std::string label = name + "step " + std::to_string(i++) + " " + op;
      if (op == "now") clock.t = s["t"].get<double>();
      else if (op == "answer") answer = s;
      else if (op == "put") files.put(dir + "\\" + str(s["name"]), unhex(str(s["hex"])), s["mtime"].get<double>());
      else if (op == "rename") check(label, files.rename(dir + "\\" + str(s["name"]), dir + "\\" + str(s["to"])));
      else if (op == "remove") check(label, files.remove(dir + "\\" + str(s["name"])));
      else if (op == "drain" || op == "start") {
        opened = json::array();
        if (op == "start") box.start();
        else { const json got = box.drain(); check(label + ": the acks", got == s["out"], brief(got.dump()) + "\n      want " + brief(s["out"].dump())); }
        check(label + ": what open() was handed", opened == s["opened"], brief(opened.dump()) + "\n      want " + brief(s["opened"].dump()));
        json names = json::array(), want = json::array();
        for (const std::string& n : files.list(dir)) names.push_back(n);
        for (const json& e : s["files"]) want.push_back(e["name"]);
        check(label + ": the folder", names == want, names.dump() + "\n      want " + want.dump());
        for (const json& e : s["files"]) {
          const std::string p = dir + "\\" + str(e["name"]);
          if (!files.has(p)) continue;
          check(label + ": " + str(e["name"]), files.get(p) == str(e["text"]), brief(files.get(p)) + "\n      want " + brief(str(e["text"])));
          const double mtime = files.stat(p).mtimeMs;
          check(label + ": mtime of " + str(e["name"]), std::fabs(mtime - e["mtime"].get<double>()) < 0.5, std::to_string(mtime) + "  want " + e["mtime"].dump());
        }
        if (op == "start") check(label + ": the folder is made", files.isDir(dir));
      }
    }
  }

  // ---- what only C++ can hand it
  { fake::Clock clock; clock.t = 1794859230000.0;
    fake::MemFiles files; files.clock = &clock;
    const std::string dir = "C:\\userData\\desk-open", id = "security-20261116T200000Z-beef", sid = "0f3c2a9e-1b2c-4d5e-8f90-123456789abc";
    const json request = { { "kind", "desk-open" }, { "id", id }, { "desk", "security" }, { "account", "C:/a" }, { "cwd", "C:/b" }, { "sessionId", sid }, { "prompt", "hello" } };
    int mode = 0;
    DeskOpenInbox box(files, clock, dir, [&](const json&) -> json {
      if (mode == 1) throw 5;                              // not a std::exception
      return { { "ok", true }, { "pid", 7.0 }, { "window", 2.0 }, { "cells", { 1.0, 2.5 } }, { "tileId", "t" } };
    });
    files.put(dir + "\\" + id + ".request.json", request.dump());
    box.drain();
    const std::string ack = files.get(dir + "\\" + id + ".ack.json");
    check("deskopen inbox: a whole number held as a double is written as JSON.stringify writes it", ack == "{\"id\":\"" + id + "\",\"ok\":true,\"sessionId\":null,\"tileId\":\"t\",\"pid\":7,\"window\":2,\"primary\":false,\"cells\":[1,2.5],\"newWindow\":false,\"how\":\"opened\",\"at\":\"2026-11-16T20:00:30.000Z\"}", ack);
    check("deskopen inbox: the log's words for it", files.get(dir + "\\log.jsonl") == "{\"at\":\"2026-11-16T20:00:30.000Z\",\"id\":\"" + id + "\",\"desk\":\"security\",\"account\":\"C:/a\",\"ok\":true,\"how\":\"opened\",\"where\":\"auxiliary window 2, cell 1+2.5\",\"tile\":\"t\",\"pid\":7}\n", files.get(dir + "\\log.jsonl"));
    check("deskopen inbox: the answer is written whole (no temp left)", !files.has(dir + "\\" + id + ".ack.json.tmp") && !files.has(dir + "\\" + id + ".taken.json") && !files.has(dir + "\\" + id + ".request.json"));
    mode = 1;
    files.put(dir + "\\" + id + ".request.json", request.dump());
    const std::vector<json> out = box.drain();
    check("deskopen inbox: open() throwing something that is not a std::exception is a refusal", out.size() == 1 && out[0]["ok"] == false && out[0]["error"] == "refused", out.empty() ? "" : out[0].dump());
    DeskOpenInbox none(files, clock, dir, nullptr);
    files.put(dir + "\\" + id + ".request.json", request.dump());
    const std::vector<json> out2 = none.drain();
    check("deskopen inbox: no open() at all: nothing opened", out2.size() == 1 && out2[0]["error"] == "nothing opened", out2.empty() ? "" : out2[0].dump());
    DeskOpenInbox gone(files, clock, "C:\\userData\\nowhere", nullptr);
    check("deskopen inbox: a folder that is not there drains nothing", gone.drain().empty()); }
}
