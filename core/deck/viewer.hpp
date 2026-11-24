// core/deck/viewer.hpp — lib/viewer.js, ported: the phone's page viewer (the pages in Settings -> Phone -> "Pages
// on the phone", and any .html file a session wrote, opened on the phone), plus what lib/phone.js serves it with
// (VIEW_CSP, VIEW_TYPES, PAGE_LINKS / withPageLinks) and main.js cleanPages. PURE over one seam: Files (stat).
//
// The rule for what is served (lib/viewer.js): a folder is shown only after the deck was asked to show an .html
// file in it. From a shown folder, its .html files and what they load (styles, scripts, images, fonts) in it or
// below it; never a dotfile or a dot folder, never '..', never a data file.
//
// Checked against the REAL lib/viewer.js (core/test/deck/gen-viewer.js -> core/test/fixtures/deck/viewer.json).
#pragma once
#include "seams.hpp"
#include <string>
#include <utility>
#include <vector>

namespace deck { namespace core {

// ---- lib/viewer.js
std::vector<std::string> viewerPathsIn(const std::string& text);                           // pathsIn (PATH_RE)
// resolvePath(raw, { home, platform }): the written form made absolute and native; "" = null. `home` is always
// given here (the JS falls back to os.homedir(); the core has no machine of its own).
std::string viewerResolvePath(const std::string& raw, const std::string& home, bool win);
// htmlFile: the .html file a path names; throws std::runtime_error with the JS's words
std::string viewerHtmlFile(Files& files, const std::string& raw, const std::string& home, bool win);

class Viewer {
 public:
  explicit Viewer(bool win = true);
  std::string idFor(const std::string& dir) const;                                         // 16 hex of sha256(normalized dir, lower-cased on Windows)
  json show(const std::string& file);                                                      // { "dir": id, "name": basename }: its folder becomes servable
  // the file behind /view/<key>/<id>/<parts...>; "" = null
  std::string fileFor(Files& files, const std::string& id, const std::vector<std::string>& parts) const;
  // this.dirs, for tests and diagnostics: how many folders are shown, is this id one, the ids least recently shown first
  size_t size() const { return dirs_.size(); }
  bool has(const std::string& id) const;
  std::vector<std::string> ids() const;
 private:
  bool win_;
  std::vector<std::pair<std::string, std::string>> dirs_;   // folder id -> folder, least recently shown first (MAX_DIRS)
};

// sessionHtml(jsonl, { home, platform }): [{ path, name, folder, mtime, size }], newest mention first, at most 40
json viewerSessionHtml(Files& files, const std::string& jsonl, const std::string& home, bool win);

// ---- main.js cleanPages, with path.isAbsolute / path.basename of the platform asked for. core/deck/rules.hpp has
// the one-argument cleanPages(list) that applySettings uses (Windows only, not in any fixture); this overload is
// the one the fixtures check.
json cleanPages(const json& list, bool win);

// ---- lib/phone.js, the /view/ route's constants
extern const char* const kViewCsp;                                                         // VIEW_CSP
extern const char* const kPageLinks;                                                       // PAGE_LINKS
const char* viewContentType(const std::string& extLower);                                  // VIEW_TYPES[ext] || 'application/octet-stream'
std::string withPageLinks(const std::string& html);                                        // PAGE_LINKS before </body\s*> (any case), else appended

// ---- Node's path module, as far as the deck leans on it (path.win32 when `win`, else path.posix), and three
// JavaScript behaviours. Here because viewer.cpp needs them exact; core/deck/deskopen.cpp uses them too.
// NOT ported from Node 24's path.win32.normalize: the reserved DOS device names ("CON:", "\\?\COM1:") and a bare
// "\\.\" or "\\?\" root. Drive, UNC, rooted and relative paths (with the "a:b" -> ".\a:b" rule) are.
std::string viewerPathNormalize(const std::string& p, bool win);
std::string viewerPathJoin(const std::vector<std::string>& parts, bool win);
std::string viewerPathDirname(const std::string& p, bool win);
std::string viewerPathBasename(const std::string& p, bool win);
std::string viewerPathExtname(const std::string& p, bool win);                             // not lower-cased
bool viewerPathIsAbsolute(const std::string& p, bool win);
// s.toLowerCase() for ASCII, Latin-1 (U+00C0..U+00DE) and Cyrillic (U+0400..U+042F); every other character is
// left as it is (JavaScript lower-cases all of Unicode).
std::string viewerLowerCase(const std::string& s);
// buf.toString('utf8'): ill-formed UTF-8 becomes U+FFFD the way Node decodes it; well-formed text is unchanged
std::string viewerUtf8(const std::string& bytes);
// String(v) for a JSON value: an array joins its elements with ',', an object is "[object Object]"
std::string viewerJsString(const json& v);

}}  // namespace deck::core
