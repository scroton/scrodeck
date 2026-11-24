// core/test/deck/viewer_test.cpp — core/deck/viewer.cpp against what the REAL lib/viewer.js (and the /view/ route's
// constants in lib/phone.js, and main.js cleanPages) answered: core/test/fixtures/deck/viewer.json, written by
// core/test/deck/gen-viewer.js. The fixture's synthetic files are loaded into fake::MemFiles at the same paths.
#include "t.hpp"
#include "fakes.hpp"
#include "../../deck/viewer.hpp"

void testViewer(const std::string& fixtures);

namespace {

using namespace deck::core;

// Windows finds a file whatever the case of the path asked for; MemFiles' keys are exact. Keys are folded here.
struct FoldFiles : fake::MemFiles {
  bool fold = true;
  std::string key(const std::string& p) const { return fold ? viewerLowerCase(p) : p; }
  void add(const std::string& path, bool dir, uint64_t size, double mtime) { if (dir) makeDirs(key(path)); else put(key(path), std::string((size_t)size, 'x'), mtime); }
  FileStat stat(const std::string& p) override { return fake::MemFiles::stat(key(p)); }
};
std::string shortOf(const std::string& s) { return s.size() > 70 ? s.substr(0, 70) + "..." : s; }
std::string orEmpty(const json& v) { return v.is_string() ? v.get<std::string>() : std::string(); }   // null = ""
std::vector<std::string> strings(const json& a) { std::vector<std::string> out; for (const json& x : a) out.push_back(x.get<std::string>()); return out; }

}  // namespace

void testViewer(const std::string& fixtures) {
  if (!want("viewer")) return;
  const json f = loadFixture(fixtures, "viewer");

  // ---- pathsIn (PATH_RE)
  { int i = 0;
    for (const json& c : f["paths"]) {
      const std::string text = str(c["text"]);
      const json got = viewerPathsIn(text);
      check("viewer pathsIn #" + std::to_string(i++) + " " + shortOf(text), got == c["out"], got.dump() + "  want " + c["out"].dump());
    } }

  // ---- Node's path, both platforms
  for (const json& c : f["pathFns"]) {
    const std::string p = str(c["p"]);
    for (const bool win : { true, false }) {
      const json& w = c[win ? "win" : "posix"];
      const std::string tag = std::string(win ? "win32 " : "posix ") + json(p).dump();
      std::string got = viewerPathNormalize(p, win);
      check("viewer path.normalize " + tag, got == str(w["normalize"]), got + "  want " + str(w["normalize"]));
      got = viewerPathDirname(p, win);
      check("viewer path.dirname " + tag, got == str(w["dirname"]), got + "  want " + str(w["dirname"]));
      got = viewerPathBasename(p, win);
      check("viewer path.basename " + tag, got == str(w["basename"]), got + "  want " + str(w["basename"]));
      got = viewerPathExtname(p, win);
      check("viewer path.extname " + tag, got == str(w["extname"]), got + "  want " + str(w["extname"]));
      check("viewer path.isAbsolute " + tag, viewerPathIsAbsolute(p, win) == w["isAbsolute"].get<bool>());
    }
  }
  for (const json& c : f["joins"]) {
    const std::vector<std::string> parts = strings(c["parts"]);
    std::string got = viewerPathJoin(parts, true);
    check("viewer path.win32.join " + c["parts"].dump(), got == str(c["win"]), got + "  want " + str(c["win"]));
    got = viewerPathJoin(parts, false);
    check("viewer path.posix.join " + c["parts"].dump(), got == str(c["posix"]), got + "  want " + str(c["posix"]));
  }
  for (const json& c : f["lower"]) { const std::string got = viewerLowerCase(str(c["s"])); check("viewer toLowerCase " + str(c["s"]), got == str(c["out"]), got); }
  for (const json& c : f["utf8"]) { const std::string got = viewerUtf8(unhex(str(c["hex"]))); check("viewer buf.toString('utf8') " + str(c["hex"]), got == unhex(str(c["out"])), got); }
  for (const json& c : f["jsString"]) { const std::string got = viewerJsString(c["v"]); check("viewer String(" + c["v"].dump() + ")", got == str(c["out"]), got); }

  // ---- resolvePath
  { int i = 0;
    for (const json& c : f["resolve"]) {
      const std::string got = viewerResolvePath(str(c["raw"]), str(c["home"]), c["win"].get<bool>());
      check("viewer resolvePath #" + std::to_string(i++) + (c["win"].get<bool>() ? " win32 " : " posix ") + c["raw"].dump() + " home " + str(c["home"]), got == orEmpty(c["out"]), got + "  want " + c["out"].dump());
    } }
  for (const json& c : f["ids"]) {
    std::string got = Viewer(true).idFor(str(c["dir"]));
    check("viewer idFor win32 " + c["dir"].dump(), got == str(c["win"]), got);
    got = Viewer(false).idFor(str(c["dir"]));
    check("viewer idFor posix " + c["dir"].dump(), got == str(c["posix"]), got);
  }

  // ---- the three worlds: htmlFile, the folder registry, a session's pages
  for (const json& w : f["worlds"]) {
    const std::string name = "viewer [" + str(w["name"]) + "] ";
    const bool win = w["win"].get<bool>();
    const std::string home = str(w["home"]);
    FoldFiles files; files.fold = win;
    for (const json& e : w["entries"]) files.add(str(e["path"]), e["dir"].get<bool>(), e["size"].get<uint64_t>(), e["mtime"].get<double>());
    check(name + "the files are loaded", files.files.size() > 60, std::to_string(files.files.size()));

    for (const json& c : w["htmlFile"]) {
      std::string got, error;
      try { got = viewerHtmlFile(files, str(c["raw"]), home, win); } catch (const std::runtime_error& e) { error = e.what(); }
      const std::string label = name + "htmlFile " + shortOf(c["raw"].dump());
      // (what() is a C string: the words stop at a NUL the path held)
      if (c.contains("error")) check(label, got.empty() && error == std::string(str(c["error"]).c_str()), "got " + got + " / " + error + "  want error " + str(c["error"]));
      else check(label, error.empty() && got == str(c["out"]), "got " + got + " / " + error + "  want " + str(c["out"]));
    }

    Viewer v(win);
    int i = 0;
    for (const json& s : w["viewer"]["steps"]) {
      const std::string op = str(s["op"]);
      const std::string label = name + "step " + std::to_string(i++) + " " + op + " ";
      if (op == "idFor") { const std::string got = v.idFor(str(s["dir"])); check(label + s["dir"].dump(), got == str(s["out"]), got); }
      else if (op == "show") { const json got = v.show(str(s["file"])); check(label + s["file"].dump(), got == s["out"], got.dump() + "  want " + s["out"].dump()); }
      else if (op == "fileFor") { const std::string got = v.fileFor(files, str(s["id"]), strings(s["parts"])); check(label + s["parts"].dump(), got == orEmpty(s["out"]), got + "  want " + s["out"].dump()); }
    }
    check(name + "the registry after the steps", v.size() == w["viewer"]["after"]["size"].get<size_t>() && json(v.ids()) == w["viewer"]["after"]["ids"], json(v.ids()).dump());
    { const json& l = w["viewer"]["lru"];
      Viewer lru(win);
      for (const json& file : l["shown"]) lru.show(str(file));
      check(name + "the registry keeps the newest 300 folders", lru.size() == 300 && lru.size() == l["size"].get<size_t>(), std::to_string(lru.size()));
      check(name + "...the same ones, least recently shown first", json(lru.ids()) == l["ids"], json(lru.ids()).dump().substr(0, 300));
      check(name + "...the first is gone, the one shown again is kept", !lru.has(str(l["first"])) && lru.has(str(l["kept"]))); }

    for (const json& s : w["sessions"]) {
      const std::string jsonl = s.contains("hex") ? unhex(str(s["hex"])) : str(s["jsonl"]);
      const json got = viewerSessionHtml(files, jsonl, str(s["home"]), win);
      check(name + "sessionHtml: " + str(s["label"]), got == s["out"], got.dump() + "\n      want " + s["out"].dump());
      bool ordered = got.is_array();
      for (const json& row : got) ordered = ordered && row.size() == 5 && row["path"].is_string() && row["name"].is_string() && row["folder"].is_string() && row["mtime"].is_number() && row["size"].is_number();
      check(name + "sessionHtml rows have path, name, folder, mtime, size: " + str(s["label"]), ordered);
    }
  }

  // ---- main.js cleanPages
  { int i = 0;
    for (const json& c : f["clean"]) {
      json got = cleanPages(c["list"], true);
      check("viewer cleanPages win32 #" + std::to_string(i), got == c["win"], got.dump() + "  want " + c["win"].dump());
      got = cleanPages(c["list"], false);
      check("viewer cleanPages posix #" + std::to_string(i++), got == c["posix"], got.dump() + "  want " + c["posix"].dump());
    }
    check("viewer cleanPages: not a list is no pages", cleanPages(json(), true).empty() && cleanPages(json("C:\\a.html"), true).empty() && cleanPages(json::object(), false).empty()); }

  // ---- lib/phone.js: what the /view/ route serves with
  { const json& p = f["phone"];
    check("viewer VIEW_CSP", std::string(kViewCsp) == str(p["VIEW_CSP"]), kViewCsp);
    check("viewer PAGE_LINKS", std::string(kPageLinks) == str(p["PAGE_LINKS"]), kPageLinks);
    for (auto it = p["VIEW_TYPES"].begin(); it != p["VIEW_TYPES"].end(); ++it) check("viewer VIEW_TYPES " + it.key(), std::string(viewContentType(it.key())) == str(it.value()), viewContentType(it.key()));
    for (const char* ext : { "", ".json", ".txt", ".HTML", "html", ".htmlx", ".exe", ".map" }) check(std::string("viewer VIEW_TYPES: anything else is a download ") + ext, std::string(viewContentType(ext)) == "application/octet-stream");
    int i = 0;
    for (const json& c : p["pages"]) { const std::string got = withPageLinks(str(c["html"])); check("viewer withPageLinks #" + std::to_string(i++) + " " + shortOf(c["html"].dump()), got == str(c["out"]), got); } }
}
