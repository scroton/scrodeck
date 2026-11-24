// windows/src/config.hpp — what the native deck remembers on THIS machine: %APPDATA%\claude-deck-native\ (the
// edition's DECK_EDITION_STATE_DIR, core/deck/edition.hpp).
//
// Its own folder, never the Electron deck's (%APPDATA%\claude-deck): the two can run side by side and
// neither reads the other's state. The deck link's token is kept DPAPI-protected (CryptProtectData, the
// signed-in Windows user's key), so the file is useless on another account or another machine.
#pragma once
#include "gfx.hpp"
#include "ws.hpp"
#include "../../core/deck/edition.hpp"
#include "../../core/third_party/nlohmann/json.hpp"
#include <wincrypt.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

namespace deck {

inline std::wstring configDir() {
  wchar_t* p = nullptr; std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) { dir = p; CoTaskMemFree(p); }
  else { wchar_t buf[MAX_PATH]; GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH); dir = buf; }
  dir += L"\\" DECK_EDITION_STATE_DIR;   // the edition's folder (core/deck/edition.hpp): claude-deck-native here
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

inline std::string b64(const BYTE* data, DWORD n) {
  DWORD len = 0; CryptBinaryToStringA(data, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
  std::string s(len, '\0'); CryptBinaryToStringA(data, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &s[0], &len);
  s.resize(len); return s;
}
inline std::string unb64(const std::string& s) {
  DWORD len = 0; if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) return "";
  std::string out(len, '\0'); CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, (BYTE*)&out[0], &len, nullptr, nullptr);
  out.resize(len); return out;
}
inline std::string protect(const std::string& plain) {
  DATA_BLOB in = { (DWORD)plain.size(), (BYTE*)plain.data() }, out = {};
  if (!CryptProtectData(&in, L"claude-deck-native link", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
  std::string s = b64(out.pbData, out.cbData); LocalFree(out.pbData); return s;
}
inline std::string unprotect(const std::string& stored) {
  std::string raw = unb64(stored); if (raw.empty()) return "";
  DATA_BLOB in = { (DWORD)raw.size(), (BYTE*)raw.data() }, out = {};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
  std::string s((char*)out.pbData, out.cbData); LocalFree(out.pbData); return s;
}

struct Config {
  nlohmann::json j = nlohmann::json::object();
  std::wstring path;

  // `dir`: keep everything in this folder instead (--state-dir; every test passes a temp folder, so a test never
  // touches the real one)
  void load(const std::wstring& dir = L"") {
    if (!dir.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    path = (dir.empty() ? configDir() : dir) + L"\\config.json";
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    auto parsed = nlohmann::json::parse(ss.str(), nullptr, false);
    if (parsed.is_object()) j = parsed;
  }
  void save() const {
    if (path.empty()) return;
    std::wstring tmp = path + L".tmp";
    { std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc); if (!f) return; f << j.dump(2); }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
  bool link(DeckLink& out) const {
    if (!j.contains("link") || !j["link"].is_object()) return false;
    const auto& l = j["link"];
    out.host = widen(l.value("host", std::string()));
    out.port = l.value("port", 443);
    out.secure = l.value("secure", true);
    out.token = unprotect(l.value("token", std::string()));
    return out.valid();
  }
  void setLink(const DeckLink& l) {
    j["link"] = { { "host", narrow(l.host) }, { "port", l.port }, { "secure", l.secure }, { "token", protect(l.token) } };
  }
  void forgetLink() { j.erase("link"); }
};

}  // namespace deck
