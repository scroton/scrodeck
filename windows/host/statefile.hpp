// windows/host/statefile.hpp — the Win32 file helpers the native deck runs on (UTF-8 paths in, wide Win32 calls
// underneath), and with them the core's Files and Clock seams (core/deck/seams.hpp) as Windows implements them.
// lib/statefile.js itself — the backup history, the unreadable-file recovery — is core/deck/statefile.hpp now
// (pure, over those seams); host::StateFile is that class on the Win32 files, for the callers that had it here.
#pragma once
#include "../../core/deck/seams.hpp"
#include "../../core/deck/statefile.hpp"
#include "../../core/deck/util.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace deck { namespace host {

using json = nlohmann::json;

// ---- files (paths are UTF-8)
bool readFile(const std::string& path, std::string& out);                 // whole file; false when it cannot be read
bool readFileFrom(const std::string& path, uint64_t offset, std::string& out, uint64_t* sizeNow = nullptr);   // [offset, end)
bool fileSize(const std::string& path, uint64_t& size);                   // false when missing or a folder
bool isFile(const std::string& path);
bool isDir(const std::string& path);
bool makeDirs(const std::string& path);
bool removeFile(const std::string& path);
bool copyFile(const std::string& from, const std::string& to);
std::vector<std::string> listDir(const std::string& dir);                 // names, sorted (as readdir gives them on NTFS)
bool writeFile(const std::string& path, const std::string& bytes);        // fs.writeFileSync
double nowMs();                                                           // Date.now()
// fs.writeFileSync through a temp file: flushed, then renamed over `path` (retried on a sharing violation,
// then written in place as the last resort). False when even that failed.
bool writeAtomic(const std::string& path, const std::string& text);

using core::joinPath;
using core::dirName;
using core::baseName;
using core::isoStamp;
using core::isoTime;
using core::parseState;

class WinFiles : public core::Files {
 public:
  bool read(const std::string& path, std::string& out) override;
  bool readFrom(const std::string& path, uint64_t offset, std::string& out, uint64_t* sizeNow) override;
  bool readRange(const std::string& path, uint64_t offset, uint64_t length, std::string& out) override;
  core::FileStat stat(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;
  bool makeDirs(const std::string& path) override;
  bool remove(const std::string& path) override;
  bool copy(const std::string& from, const std::string& to) override;
  bool writeAtomic(const std::string& path, const std::string& text) override;
  bool write(const std::string& path, const std::string& bytes) override;
  bool rename(const std::string& from, const std::string& to) override;   // MoveFileExW, replacing: atomic on one volume
};
class WinClock : public core::Clock {
 public:
  double nowMs() override;
  double localOffsetMs(double utcMs) override;
};
WinFiles& winFiles();
WinClock& winClock();

class StateFile : public core::StateFile {
 public:
  StateFile(std::string file, CountFn count, json empty, double everyMs = 10 * 60000.0, int keep = 30, int days = 7)
    : core::StateFile(winFiles(), winClock(), std::move(file), std::move(count), std::move(empty), everyMs, keep, days) {}
};

}}  // namespace deck::host
