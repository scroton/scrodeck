// core/deck/statefile.hpp — lib/statefile.js, ported: a JSON state file a crash cannot take away. PURE over the
// Files and Clock seams (the atomic write itself is the shell's: Files::writeAtomic).
//   1. every write is atomic;
//   2. the file keeps a history in <name>-backups/: a copy before a write replaces it, at most once every
//      `everyMs`, and ALWAYS when the write would leave fewer items than the file holds; the newest `keep`
//      copies stay, plus the newest copy of each of the last `days` days;
//   3. a file that exists but will not parse is never read as empty: loadAtBoot() copies it aside as
//      <file>.unreadable-<time> and loads the newest backup that parses.
#pragma once
#include "seams.hpp"
#include <functional>
#include <string>
#include <vector>

namespace deck { namespace core {

class StateFile {
 public:
  using CountFn = std::function<size_t(const json&)>;
  StateFile(Files& files, Clock& clock, std::string file, CountFn count, json empty, double everyMs = 10 * 60000.0, int keep = 30, int days = 7);
  json current() const;                   // the file as it stands, or a discarded value
  json read() const;                      // the file, or the empty state
  std::vector<std::string> backups() const;   // newest first
  std::string backup(const std::string& why = "");
  void write(const json& data);
  struct Boot { json data; std::string recoveredFrom, unreadableCopy; };
  Boot loadAtBoot();
  const std::string& file() const { return file_; }
 private:
  void prune();
  Files& files_; Clock& clock_;
  std::string file_, dir_, name_;
  CountFn count_; json empty_;
  double everyMs_; int keep_, days_;
  double lastBackupAt_ = 0;
};

}}  // namespace deck::core
