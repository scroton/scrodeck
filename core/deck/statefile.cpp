// core/deck/statefile.cpp — see statefile.hpp.
#include "statefile.hpp"
#include "util.hpp"
#include <algorithm>
#include <set>

namespace deck { namespace core {

StateFile::StateFile(Files& files, Clock& clock, std::string file, CountFn count, json empty, double everyMs, int keep, int days)
  : files_(files), clock_(clock), file_(std::move(file)), count_(std::move(count)), empty_(std::move(empty)), everyMs_(everyMs), keep_(keep), days_(days) {
  const std::string base = baseName(file_);
  const size_t dot = base.rfind('.');
  name_ = dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
  dir_ = joinPath(dirName(file_), name_ + "-backups");
}

json StateFile::current() const {
  std::string text;
  if (!files_.read(file_, text)) return json(json::value_t::discarded);
  return parseState(text);
}
json StateFile::read() const { json v = current(); return v.is_discarded() ? empty_ : v; }

std::vector<std::string> StateFile::backups() const {
  std::vector<std::string> names;
  for (const std::string& n : files_.list(dir_)) if (n.size() >= 5 && n.compare(n.size() - 5, 5, ".json") == 0) names.push_back(n);
  std::sort(names.begin(), names.end());
  std::reverse(names.begin(), names.end());
  std::vector<std::string> out;
  for (const std::string& n : names) out.push_back(joinPath(dir_, n));
  return out;
}

std::string StateFile::backup(const std::string& why) {
  std::string text;
  if (!files_.read(file_, text)) return std::string();
  if (parseState(text).is_discarded()) return std::string();            // never keep a broken file as a "good copy"
  files_.makeDirs(dir_);
  const std::string to = joinPath(dir_, name_ + "-" + isoStamp(clock_.nowMs()) + (why.empty() ? "" : "-" + why) + ".json");
  files_.writeAtomic(to, text);
  lastBackupAt_ = clock_.nowMs();
  prune();
  return to;
}

void StateFile::prune() {
  const std::vector<std::string> all = backups();
  std::set<std::string> keepSet;
  for (size_t i = 0; i < all.size() && (int)i < keep_; i++) keepSet.insert(all[i]);
  std::set<std::string> seenDay;
  const std::string cutoff = isoStamp(clock_.nowMs() - days_ * 86400000.0).substr(0, 10);
  for (const std::string& f : all) {                                    // newest first, so the first per day wins
    const std::string b = baseName(f);
    // /(\d{4}-\d{2}-\d{2})T/ : the first date followed by a T
    std::string day;
    for (size_t i = 0; i + 10 < b.size(); i++) {
      bool ok = b[i + 10] == 'T' && b[i + 4] == '-' && b[i + 7] == '-';
      for (size_t k : { 0, 1, 2, 3, 5, 6, 8, 9 }) if (ok && !(b[i + k] >= '0' && b[i + k] <= '9')) ok = false;
      if (ok) { day = b.substr(i, 10); break; }
    }
    if (day.empty() || day < cutoff || seenDay.count(day)) continue;
    seenDay.insert(day); keepSet.insert(f);
  }
  for (const std::string& f : all) if (!keepSet.count(f)) files_.remove(f);
}

void StateFile::write(const json& data) {
  const json prev = current();
  if (!prev.is_discarded()) {
    const bool dropped = count_(data) < count_(prev);
    if (dropped || clock_.nowMs() - lastBackupAt_ >= everyMs_) backup(dropped ? "drop" : "");
  }
  files_.writeAtomic(file_, data.dump(2, ' ', false, json::error_handler_t::replace));
}

StateFile::Boot StateFile::loadAtBoot() {
  Boot out;
  if (!files_.isFile(file_)) { out.data = empty_; return out; }
  json data = current();
  if (!data.is_discarded()) { backup("boot"); out.data = data; return out; }
  out.unreadableCopy = file_ + ".unreadable-" + isoStamp(clock_.nowMs());
  if (!files_.copy(file_, out.unreadableCopy)) out.unreadableCopy.clear();
  for (const std::string& f : backups()) {
    std::string text;
    if (!files_.read(f, text)) continue;
    json d = parseState(text);
    if (d.is_discarded()) continue;
    files_.writeAtomic(file_, d.dump(2, ' ', false, json::error_handler_t::replace));
    out.data = d; out.recoveredFrom = f;
    return out;
  }
  out.data = empty_;
  return out;
}

}}  // namespace deck::core
