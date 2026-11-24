// core/deck/platform.cpp — see platform.hpp. lib/platform.js, function for function.
#include "platform.hpp"
#include "util.hpp"
#include "../session/jsutil.hpp"

namespace deck { namespace core { namespace platform {

namespace js = deck::session::js;

static std::string join3(const std::string& a, const std::string& b, const std::string& c, const std::string& d = "") {
  std::string p = joinPath(joinPath(a, b), c);
  return d.empty() ? p : joinPath(p, d);
}

std::string claudeExe(Env& env, Files& files, const std::string& override_) {
  if (!override_.empty()) return override_;
  const bool win = env.os() == "win32";
  const std::string home = env.home();
  const std::string local = join3(home, ".local", "bin", win ? "claude.exe" : "claude");
  if (files.isFile(local)) return local;
  if (!win) for (const std::string& p : { std::string("/opt/homebrew/bin/claude"), std::string("/usr/local/bin/claude"), join3(home, ".npm-global", "bin", "claude") }) if (files.isFile(p)) return p;
  return win ? "claude.exe" : "claude";     // a Path lookup
}

Shell shellCommand(Env& env) {
  if (env.os() == "win32") return { "powershell.exe", { "-NoLogo" }, "PowerShell" };
  const std::string sh = !env.get("SHELL").empty() ? env.get("SHELL") : std::string("/bin/zsh");
  return { sh, { "-l" }, baseName(sh) };
}

std::string bashPath(Env& env, Files& files, const std::string& override_) {
  if (!override_.empty()) return override_;
  if (env.os() != "win32") return files.isFile("/bin/bash") ? "/bin/bash" : "bash";
  const std::string pf = !env.get("ProgramFiles").empty() ? env.get("ProgramFiles") : std::string("C:\\Program Files");
  const std::string pf86 = !env.get("ProgramFiles(x86)").empty() ? env.get("ProgramFiles(x86)") : std::string("C:\\Program Files (x86)");
  for (const std::string& p : { join3(pf, "Git", "bin", "bash.exe"), join3(pf86, "Git", "bin", "bash.exe"), joinPath(join3(env.get("LOCALAPPDATA"), "Programs", "Git", "bin"), "bash.exe") })
    if (files.isFile(p)) return p;
  return "bash.exe";
}

std::string userswitchPath(Env& env, Files& files, const std::string& appDir, const std::string& resourcesDir) {
  std::vector<std::string> c = { join3(env.home(), ".local", "bin", "userswitch") };
  if (!resourcesDir.empty()) c.push_back(join3(resourcesDir, "bin", "userswitch"));
  c.push_back(join3(appDir, "bin", "userswitch"));
  for (const std::string& p : c) if (files.isFile(p)) return p;
  return join3(appDir, "bin", "userswitch");
}

AliasFiles aliasFiles(Env& env, Files& files) {
  AliasFiles out;
  out.primary = !env.get("USERSWITCH_ZSHRC").empty() ? env.get("USERSWITCH_ZSHRC") : joinPath(env.home(), ".claude-code-account-aliases");
  if (env.os() != "win32") for (const std::string& f : { joinPath(env.home(), ".zshrc"), joinPath(env.home(), ".bashrc") }) if (f != out.primary && files.isFile(f)) out.extra.push_back(f);
  return out;
}

json keyHelp(const std::string& os) {
  if (os == "darwin") return { { "sidebar", "Cmd+B" }, { "fullscreen", "Cmd+Ctrl+F" }, { "prev", "Cmd+Shift+[" }, { "next", "Cmd+Shift+]" }, { "palette", "Cmd+Shift+P" },
    { "settings", "Cmd+," }, { "redraw", "Cmd+R" }, { "zoom", "Cmd+Shift+Enter" }, { "asks", "Cmd+Shift+A" } };
  return { { "sidebar", "F9 / Ctrl+`" }, { "fullscreen", "F10" }, { "prev", "F11" }, { "next", "F12" }, { "palette", "Ctrl+Shift+P" }, { "redraw", "F5" }, { "zoom", "F6" }, { "asks", "F7" } };
}
std::string defaultFontStack(const std::string& os) {
  return os == "darwin" ? "\"Cascadia Mono\", \"SF Mono\", Menlo, Monaco, monospace" : "\"Cascadia Mono\", \"Cascadia Code\", Consolas, monospace";
}

std::string keyAction(const std::string& os, const json& input) {
  const std::string key = js::str(js::get(input, "key")), k = lowerAscii(key), code = js::str(js::get(input, "code"));
  const bool meta = js::truthy(js::get(input, "meta")), shift = js::truthy(js::get(input, "shift")), alt = js::truthy(js::get(input, "alt")), control = js::truthy(js::get(input, "control"));
  if (os == "darwin") {
    if (meta && shift && !alt && k == "p") return "palette";
    if (meta && !shift && !alt && !control && k == "b") return "sidebar";
    if (meta && control && k == "f") return "fullscreen";
    if (meta && shift && (k == "[" || k == "{")) return "prev";
    if (meta && shift && (k == "]" || k == "}")) return "next";
    if (meta && alt && k == "i") return "devtools";
    if (meta && !shift && !alt && !control && k == ",") return "settings";
    if (meta && !shift && !alt && !control && k == "r") return "redraw";
    if (meta && shift && !alt && !control && k == "enter") return "zoom";
    if (meta && shift && !alt && !control && k == "a") return "asks";
    return "";
  }
  if (key == "F5") return "redraw";
  if (key == "F7" && !control && !alt && !meta) return "asks";
  if (key == "F6") return "zoom";
  if (key == "F9" || (control && !shift && !alt && code == "Backquote")) return "sidebar";
  if (key == "F10" && !shift) return "fullscreen";   // Shift+F10: the tile menu (lib/platform.js)
  if (key == "F11") return "prev";
  if (key == "F12") return "next";
  if (control && shift && !alt && k == "p") return "palette";
  if (control && shift && k == "i") return "devtools";
  return "";
}

}}}  // namespace deck::core::platform
