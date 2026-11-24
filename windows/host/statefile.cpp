// windows/host/statefile.cpp — see statefile.hpp.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "statefile.hpp"
#include "conpty.hpp"      // widen / narrow

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace deck { namespace host {

static std::wstring W(const std::string& s) { return widen(s); }

bool readFile(const std::string& path, std::string& out) { return readFileFrom(path, 0, out); }

bool readFileFrom(const std::string& path, uint64_t offset, std::string& out, uint64_t* sizeNow) {
  out.clear();
  HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size; size.QuadPart = 0;
  GetFileSizeEx(h, &size);
  if (sizeNow) *sizeNow = (uint64_t)size.QuadPart;
  if (offset < (uint64_t)size.QuadPart) {
    LARGE_INTEGER at; at.QuadPart = (LONGLONG)offset;
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    uint64_t left = (uint64_t)size.QuadPart - offset;
    out.resize((size_t)left);
    size_t got = 0;
    while (got < out.size()) {
      DWORD n = 0;
      if (!ReadFile(h, &out[got], (DWORD)std::min<uint64_t>(out.size() - got, 1 << 20), &n, nullptr) || n == 0) break;
      got += n;
    }
    out.resize(got);
  }
  CloseHandle(h);
  return true;
}

bool fileSize(const std::string& path, uint64_t& size) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(W(path).c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
  size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
  return true;
}
bool isFile(const std::string& path) { DWORD a = GetFileAttributesW(W(path).c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
bool isDir(const std::string& path) { DWORD a = GetFileAttributesW(W(path).c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
bool makeDirs(const std::string& path) {
  if (path.empty() || isDir(path)) return true;
  const std::string parent = dirName(path);
  if (!parent.empty() && parent != path && !isDir(parent)) makeDirs(parent);
  return CreateDirectoryW(W(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool removeFile(const std::string& path) { return DeleteFileW(W(path).c_str()) != 0; }
bool copyFile(const std::string& from, const std::string& to) { return CopyFileW(W(from).c_str(), W(to).c_str(), FALSE) != 0; }

std::vector<std::string> listDir(const std::string& dir) {
  std::vector<std::string> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(W(joinPath(dir, "*")).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    const std::wstring n = fd.cFileName;
    if (n != L"." && n != L"..") out.push_back(narrow(n));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}

double nowMs() {
  return (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
bool writeAtomic(const std::string& path, const std::string& text) {
  makeDirs(dirName(path));
  const std::string tmp = path + ".tmp";
  auto writeTo = [&](const std::string& target, bool flush) {
    HANDLE h = CreateFileW(W(target).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true; size_t off = 0;
    while (ok && off < text.size()) { DWORD n = 0; ok = WriteFile(h, text.data() + off, (DWORD)std::min<size_t>(text.size() - off, 1 << 20), &n, nullptr) != 0; off += n; }
    if (ok && flush) FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
  };
  if (!writeTo(tmp, true)) return writeTo(path, false);
  for (int i = 0;; i++) {
    if (MoveFileExW(W(tmp).c_str(), W(path).c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    const DWORD e = GetLastError();
    // a virus scanner or the indexer can hold the target for a moment (EPERM / EBUSY / EACCES in Node's words)
    if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) { removeFile(tmp); return false; }
    if (i >= 4) { const bool ok = writeTo(path, false); removeFile(tmp); return ok; }
    Sleep(25 * (i + 1));
  }
}

bool writeFile(const std::string& path, const std::string& bytes) {
  HANDLE h = CreateFileW(W(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = true; size_t off = 0;
  while (ok && off < bytes.size()) { DWORD n = 0; ok = WriteFile(h, bytes.data() + off, (DWORD)std::min<size_t>(bytes.size() - off, 1 << 20), &n, nullptr) != 0; off += n; }
  CloseHandle(h);
  return ok;
}

// ---------------------------------------------------------------- the core's Files and Clock seams, on Win32
bool WinFiles::read(const std::string& path, std::string& out) { return readFile(path, out); }
bool WinFiles::readFrom(const std::string& path, uint64_t offset, std::string& out, uint64_t* sizeNow) { return readFileFrom(path, offset, out, sizeNow); }
bool WinFiles::readRange(const std::string& path, uint64_t offset, uint64_t length, std::string& out) {
  out.clear();
  HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER at; at.QuadPart = (LONGLONG)offset;
  SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
  out.resize((size_t)length);
  size_t got = 0;
  while (got < out.size()) { DWORD n = 0; if (!ReadFile(h, &out[got], (DWORD)std::min<uint64_t>(out.size() - got, 1 << 20), &n, nullptr) || n == 0) break; got += n; }
  out.resize(got);
  CloseHandle(h);
  return true;
}
core::FileStat WinFiles::stat(const std::string& path) {
  core::FileStat st;
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(W(path).c_str(), GetFileExInfoStandard, &d)) return st;
  st.exists = true; st.isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
  const uint64_t ft = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;   // 100 ns since 1601
  st.mtimeMs = (double)ft / 10000.0 - 11644473600000.0;
  return st;
}
std::vector<std::string> WinFiles::list(const std::string& dir) { return listDir(dir); }
bool WinFiles::makeDirs(const std::string& path) { return host::makeDirs(path); }
bool WinFiles::remove(const std::string& path) { return removeFile(path); }
bool WinFiles::copy(const std::string& from, const std::string& to) { return copyFile(from, to); }
bool WinFiles::writeAtomic(const std::string& path, const std::string& text) { return host::writeAtomic(path, text); }
bool WinFiles::write(const std::string& path, const std::string& bytes) { return writeFile(path, bytes); }
bool WinFiles::rename(const std::string& from, const std::string& to) { return MoveFileExW(W(from).c_str(), W(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0; }
WinFiles& winFiles() { static WinFiles f; return f; }
double WinClock::nowMs() { return host::nowMs(); }
double WinClock::localOffsetMs(double utcMs) {
  // local wall clock minus UTC at that instant
  const int64_t ft = (int64_t)((utcMs + 11644473600000.0) * 10000.0);
  FILETIME f; f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFF); f.dwHighDateTime = (DWORD)(ft >> 32);
  SYSTEMTIME utc, local;
  if (!FileTimeToSystemTime(&f, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return 0;
  FILETIME lf; if (!SystemTimeToFileTime(&local, &lf)) return 0;
  const int64_t l = ((int64_t)lf.dwHighDateTime << 32) | lf.dwLowDateTime;
  return (double)(l - ft) / 10000.0;
}
WinClock& winClock() { static WinClock c; return c; }

}}  // namespace deck::host
