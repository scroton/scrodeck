// core/session/transcript.cpp — see transcript.hpp. A line-for-line port of lib/transcript.js; the comments
// there say why each rule exists (and what real transcripts it was read off), so they are not repeated here.
#include "transcript.hpp"
#include "jsutil.hpp"

namespace deck { namespace session {

using json = nlohmann::json;
using namespace js;

static const double kNaN = std::numeric_limits<double>::quiet_NaN();

std::optional<double> cronOnceAt(const std::string& exprIn, double from) {
  // String(expr || '').trim().split(/\s+/)
  const std::string expr = trim(exprIn);
  std::vector<std::string> f;
  { std::string cur; bool any = false;
    for (size_t i = 0; i < expr.size();) {
      size_t j = i; const bool sp = isSpace(nextCodePoint(expr, j));
      if (sp) { if (any) { f.push_back(cur); cur.clear(); any = false; } }
      else { cur.append(expr, i, j - i); any = true; }
      i = j;
    }
    if (any) f.push_back(cur);
    if (expr.empty()) f.push_back(""); }
  if (f.size() != 5) return std::nullopt;
  int v[4];
  for (int k = 0; k < 4; k++) {
    if (f[k].empty()) return std::nullopt;
    double n = 0;
    for (char c : f[k]) { if (c < '0' || c > '9') return std::nullopt; n = n * 10 + (c - '0'); if (n > 1e8) return std::nullopt; }
    v[k] = (int)n;
  }
  const int mi = v[0], h = v[1], dom = v[2], mon = v[3];
  const int y = localYear(from);
  for (int yr : { y, y + 1 }) {
    const double t = localToEpochMs(yr, mon - 1, dom, h, mi, 0);
    if (!std::isnan(t) && t >= from - 60000) return t;
  }
  return std::nullopt;
}

static std::string textOf(const json& content) {
  if (content.is_string()) return content.get<std::string>();
  if (!content.is_array()) return std::string();
  std::string out; bool first = true;
  for (const json& b : content) {
    if (!first) out += ' ';
    first = false;
    if (truthy(b) && get(b, "type") == "text" && get(b, "text").is_string()) out += str(get(b, "text"));
    else if (truthy(b) && get(b, "type") == "tool_result") out += textOf(get(b, "content"));
  }
  return out;
}

// ---- the newest prompt a person sent (lib/transcript.js promptKind / humanPromptText)
// PROMPT_NOT_TYPED_RE: /^\s*<(local-command-stdout|...|bash-stderr)\b/
static bool notTyped(const std::string& t) {
  size_t i = 0;
  while (i < t.size()) { size_t j = i; if (!isSpace(nextCodePoint(t, j))) break; i = j; }
  if (i >= t.size() || t[i] != '<') return false;
  for (const char* tag : { "local-command-stdout", "local-command-stderr", "local-command-caveat", "task-notification", "bash-stdout", "bash-stderr" }) {
    const size_t n = std::char_traits<char>::length(tag);
    if (t.compare(i + 1, n, tag) == 0 && (i + 1 + n >= t.size() || !isWordChar((unsigned char)t[i + 1 + n]))) return true;
  }
  return false;
}
std::string promptKind(const std::string& text) {
  // text.replace(/^(?:\s|[*`>_#]|<\/?pasted_content\b[^>]*>)+/, '')
  size_t i = 0;
  for (;;) {
    if (i >= text.size()) break;
    size_t j = i;
    if (isSpace(nextCodePoint(text, j))) { i = j; continue; }
    const char c = text[i];
    if (c == '*' || c == '`' || c == '>' || c == '_' || c == '#') { i++; continue; }
    if (c == '<') {
      size_t k = i + 1; if (k < text.size() && text[k] == '/') k++;
      static const char kTag[] = "pasted_content";
      const size_t n = sizeof(kTag) - 1;
      if (text.compare(k, n, kTag) == 0 && (k + n >= text.size() || !isWordChar((unsigned char)text[k + n]))) {
        const size_t gt = text.find('>', k + n);
        if (gt != std::string::npos) { i = gt + 1; continue; }
      }
    }
    break;
  }
  if (text.compare(i, 6, "PAUSE:") == 0) return "pause";
  if (text.compare(i, 7, "RESUME:") == 0) return "resume";
  return "other";
}
static const json& originKind(const json& o) { return (truthy(o) && (o.is_object() || o.is_array())) ? get(o, "kind") : o; }
std::optional<std::string> humanPromptText(const json& rec) {
  if (!rec.is_object()) return std::nullopt;
  const json& type = get(rec, "type");
  if (type == "attachment") {
    const json& a = get(rec, "attachment");
    if (!truthy(a) || get(a, "type") != "queued_command" || truthy(get(a, "isMeta"))) return std::nullopt;
    if (originKind(get(a, "origin")) != "human") return std::nullopt;
    const json& pr = get(a, "prompt");
    const std::string t = pr.is_string() ? str(pr) : textOf(pr);
    if (notTyped(t)) return std::nullopt;
    return t;
  }
  if (type != "user" || truthy(get(rec, "isMeta")) || truthy(get(rec, "isCompactSummary")) || truthy(get(rec, "isSidechain")) || rec.contains("toolUseResult")) return std::nullopt;
  const json& o = originKind(get(rec, "origin"));
  if (!o.is_null() && o != "human") return std::nullopt;
  const json& c = get(get(rec, "message"), "content");
  std::optional<std::string> t;
  if (c.is_string()) t = str(c);
  else if (c.is_array()) {
    bool tool = false; for (const json& b : c) if (truthy(b) && get(b, "type") == "tool_result") tool = true;
    if (!tool) {
      std::string out; bool first = true;
      for (const json& b : c) if (truthy(b) && get(b, "type") == "text" && get(b, "text").is_string()) { if (!first) out += '\n'; out += str(get(b, "text")); first = false; }
      t = out;
    }
  }
  if (!t || notTyped(*t)) return std::nullopt;
  return t;
}

// /<tag>([^<]+)<\/tag>/ : the first place the pattern holds
static bool tagValue(const std::string& s, const char* open, const char* close, std::string& out) {
  const size_t on = std::char_traits<char>::length(open);
  for (size_t p = s.find(open); p != std::string::npos; p = s.find(open, p + 1)) {
    const size_t a = p + on, lt = s.find('<', a);
    if (lt == std::string::npos || lt == a) continue;
    if (startsWith(s, close, lt)) { out = s.substr(a, lt - a); return true; }
  }
  return false;
}

void TranscriptTail::endNotified(const std::string& s) {
  static const char kOpen[] = "<task-notification>", kClose[] = "</task-notification>";
  size_t at = 0;
  for (;;) {
    const size_t a = s.find(kOpen, at);
    if (a == std::string::npos) return;
    const size_t b = s.find(kClose, a + sizeof(kOpen) - 1);
    if (b == std::string::npos) return;
    const std::string inner = s.substr(a + sizeof(kOpen) - 1, b - (a + sizeof(kOpen) - 1));
    std::string id, st;
    if (tagValue(inner, "<task-id>", "</task-id>", id) && tagValue(inner, "<status>", "</status>", st)) bg_.erase(trim(id));
    at = b + sizeof(kClose) - 1;
  }
}

void TranscriptTail::setUltracode(bool on, const json& rec) {
  f_.ultracode = on;
  const json& ts = get(rec, "timestamp");
  const double at = truthy(ts) ? (ts.is_string() ? dateParse(str(ts)) : kNaN) : kNaN;
  f_.ultracodeAt = on && !std::isnan(at) ? std::optional<double>(at) : std::nullopt;
}

static std::optional<std::string> labelOf(const std::string& s) { return s.empty() ? std::nullopt : std::optional<std::string>(s); }
static bool isBgLabelTool(const json& name) {
  if (!name.is_string()) return false;
  const std::string& n = str(name);
  return n == "Bash" || n == "PowerShell" || n == "Agent" || n == "Task" || n == "Monitor" || n == "Workflow" || n == "CronCreate";
}

void TranscriptTail::noteBackground(const json& rec, double now) {
  const json& ts = get(rec, "timestamp");
  double at = ts.is_string() ? dateParse(str(ts)) : kNaN;
  if (std::isnan(at) || at == 0) at = now;
  const json& type = get(rec, "type");
  if (type == "queue-operation") {
    if (get(rec, "operation") == "enqueue" && get(rec, "content").is_string()) endNotified(str(get(rec, "content")));
    return;
  }
  if (type == "attachment") {
    const json& a = get(rec, "attachment");
    if (truthy(a) && get(a, "type") == "queued_command") {
      const json& p = get(a, "prompt");
      endNotified(p.is_string() ? str(p) : textOf(p));
    }
    return;
  }
  if (type == "assistant") {
    const json& c = get(get(rec, "message"), "content");
    if (!c.is_array()) return;
    for (const json& b : c) {
      if (!truthy(b) || get(b, "type") != "tool_use") continue;
      static const json emptyObj = json::object();
      const json& inp = truthy(get(b, "input")) ? get(b, "input") : emptyObj;
      const json& name = get(b, "name");
      const json& id = get(b, "id");
      if (isBgLabelTool(name) && truthy(id)) {
        const json& src = orElse(get(inp, "description"), get(inp, "prompt"));
        const std::string label = sliceUnits(trim(collapseSpaces(truthy(src) ? toString(src) : std::string())), 80);
        uses_.set(toString(id), labelOf(label));
        if (uses_.items.size() > 400) uses_.items.erase(uses_.items.begin());
      }
      if (name == "CronDelete" && truthy(get(inp, "id"))) bg_.erase(toString(get(inp, "id")));
      if (name == "ScheduleWakeup") {
        double delay = 0;
        if (truthy(get(inp, "stop"))) bg_.erase("wakeup");
        else if (finiteNumber(inp, "delaySeconds", &delay)) {
          const json& reason = get(inp, "reason");
          bg_.set("wakeup", Bg{ "wakeup", labelOf(sliceUnits(truthy(reason) ? toString(reason) : std::string(), 80)), at, at + delay * 1000 + 90000, false });
        }
      }
      if (name == "SendMessage" && truthy(get(inp, "to"))) {
        const std::string to = toString(get(inp, "to"));
        if (agents_.has(to) && !bg_.has(to)) bg_.set(to, Bg{ "agent", *agents_.find(to), at, std::nullopt, false });
      }
    }
    return;
  }
  if (type != "user") return;
  const json& o = get(rec, "origin");
  const json& origin = (truthy(o) && (o.is_object() || o.is_array())) ? get(o, "kind") : o;
  if (origin == "task-notification") { endNotified(textOf(get(get(rec, "message"), "content"))); return; }
  const json& r = get(rec, "toolUseResult");
  if (!r.is_object()) return;
  const json& c = get(get(rec, "message"), "content");
  std::optional<std::string> useId;
  if (c.is_array()) {
    for (const json& b : c) if (truthy(b) && get(b, "type") == "tool_result") { const json& u = get(b, "tool_use_id"); if (truthy(u)) useId = toString(u); break; }
  }
  std::optional<std::string> label;
  if (useId) { if (auto* p = uses_.find(*useId)) label = (p->has_value() && !(*p)->empty()) ? *p : std::nullopt; uses_.erase(*useId); }
  double timeoutMs = 0;
  if (truthy(get(r, "backgroundTaskId"))) {
    bg_.set(toString(get(r, "backgroundTaskId")), Bg{ "shell", label, at, std::nullopt, false });
  } else if (get(r, "status") == "async_launched" && truthy(get(r, "taskId"))) {
    const bool workflow = get(r, "taskType") == "local_workflow" || truthy(get(r, "workflowName"));
    const json& wn = get(r, "workflowName");
    bg_.set(toString(get(r, "taskId")), Bg{ workflow ? "workflow" : "task", truthy(wn) ? std::optional<std::string>(toString(wn)) : label, at, std::nullopt, false });
  } else if (get(r, "status") == "async_launched" && truthy(get(r, "agentId"))) {
    const json& desc = get(r, "description");
    const std::optional<std::string> l = truthy(desc) ? std::optional<std::string>(toString(desc)) : label;
    bg_.set(toString(get(r, "agentId")), Bg{ "agent", l, at, std::nullopt, false });
    agents_.set(toString(get(r, "agentId")), l);
  } else if (truthy(get(r, "taskId")) && finiteNumber(r, "timeoutMs", &timeoutMs)) {
    bg_.set(toString(get(r, "taskId")), Bg{ "monitor", label, at, truthy(get(r, "persistent")) ? std::nullopt : std::optional<double>(at + timeoutMs), false });
  } else if (truthy(get(r, "id")) && get(r, "durable") == false) {
    const bool recurring = truthy(get(r, "recurring"));
    const json& hs = get(r, "humanSchedule");
    const std::optional<double> once = recurring ? std::nullopt : cronOnceAt(truthy(hs) ? toString(hs) : std::string(), at);
    bg_.set(toString(get(r, "id")), Bg{ "cron", label, at, recurring ? std::nullopt : (once ? std::optional<double>(*once + 120000) : std::nullopt), recurring });
  } else if (truthy(get(r, "task_id"))) {
    const json& m = get(r, "message");
    std::string msg = truthy(m) ? toString(m) : std::string();
    for (char& ch : msg) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    if (msg.find("stopped") != std::string::npos) bg_.erase(toString(get(r, "task_id")));
  }
}

std::vector<BackgroundWork> TranscriptTail::background(double since, double now) const {
  std::vector<BackgroundWork> out;
  for (const auto& kv : bg_.items) {
    const Bg& b = kv.second;
    if (b.at < since) continue;
    if (b.until && *b.until <= now) continue;
    out.push_back({ kv.first, b.kind, b.label, b.at, b.until, b.recurring });
  }
  return out;
}

void TranscriptTail::restart() { rest_.clear(); decoder_.reset(); }

int TranscriptTail::feed(const char* bytes, size_t n, double now) {
  std::string text = rest_ + decoder_.write(bytes, n);
  int lines = 0;
  size_t from = 0;
  for (;;) {
    const size_t nl = text.find('\n', from);
    if (nl == std::string::npos) break;
    ingest(text.substr(from, nl - from), now);
    lines++;
    from = nl + 1;
  }
  rest_ = text.substr(from);
  return lines;
}

// "X\b" at s[at]: X, then the end or a character that is not a word character
static bool wordAt(const std::string& s, size_t at, const char* x) {
  const size_t n = std::char_traits<char>::length(x);
  if (!startsWith(s, x, at)) return false;
  return at + n >= s.size() || !isWordChar((unsigned char)s[at + n]);
}
// /^[^<]*· Ultracode (on|off)\b/ : some "· Ultracode x" with no '<' anywhere before it
static bool dotUltracode(const std::string& out, const char* tail) {
  const std::string needle = std::string("\xC2\xB7 Ultracode ") + tail;
  const size_t firstLt = out.find('<');
  for (size_t p = out.find(needle); p != std::string::npos; p = out.find(needle, p + 1)) {
    if (firstLt != std::string::npos && firstLt < p) return false;
    if (wordAt(out, p, needle.c_str())) return true;
  }
  return false;
}

void TranscriptTail::ingest(const std::string& line, double now) {
  if (utf16Length(line) < 2) return;
  if (startsWith(line, "\xEF\xBB\xBF")) return;              // JSON.parse refuses a BOM; nlohmann would skip it
  const json rec = json::parse(line, nullptr, false);
  if (rec.is_discarded() || !rec.is_object()) return;
  const json& type = get(rec, "type");
  if (type == "user" || type == "assistant" || type == "queue-operation" || type == "attachment") noteBackground(rec, now);
  if (!type.is_string()) return;
  const std::string& t = str(type);
  if (t == "user" || t == "attachment") {
    const std::optional<std::string> p = humanPromptText(rec);
    if (p) {
      const json& ts = t == "attachment" ? orElse(get(get(rec, "attachment"), "timestamp"), get(rec, "timestamp")) : get(rec, "timestamp");
      const double at = ts.is_string() ? dateParse(str(ts)) : kNaN;
      f_.lastPromptKind = promptKind(*p); f_.lastPromptAt = (std::isnan(at) || at == 0) ? now : at;
    }
  }
  if (t == "ai-title") {
    const json& v = get(rec, "aiTitle");
    aiTitle = truthy(v) ? std::optional<std::string>(toString(v)) : std::nullopt;
  } else if (t == "custom-title") {
    const json& v = get(rec, "customTitle");
    customTitle = truthy(v) ? std::optional<std::string>(toString(v)) : std::nullopt;
  } else if (t == "user") {
    const json& cwd = get(rec, "cwd");
    if (truthy(cwd) && !f_.cwd) f_.cwd = toString(cwd);
    const json& ts = get(rec, "timestamp");
    if (truthy(ts)) f_.lastAt = toString(ts);
    const json& c = get(get(rec, "message"), "content");
    if (!truthy(get(rec, "isMeta")) && c.is_string()) f_.lastRole = "user";
    static const char kStdout[] = "<local-command-stdout>";
    if (c.is_string() && startsWith(str(c), kStdout)) {
      const std::string out = str(c).substr(sizeof(kStdout) - 1);
      if (wordAt(out, 0, "Ultracode off") || dotUltracode(out, "off")) setUltracode(false, rec);
      else if (wordAt(out, 0, "Ultracode on") || wordAt(out, 0, "Set effort level to ultracode") || dotUltracode(out, "on")) setUltracode(true, rec);
      else if (startsWith(out, "Current effort level:") || startsWith(out, "Effort level: auto (currently")) setUltracode(false, rec);
    }
  } else if (t == "attachment") {
    const json& a = get(rec, "attachment");
    const json& at = truthy(a) ? get(a, "type") : null();
    if (at == "ultra_effort_enter") setUltracode(true, rec);
    else if (at == "ultra_effort_exit") setUltracode(false, rec);
  } else if (t == "assistant") {
    const json& ts = get(rec, "timestamp");
    if (truthy(ts)) { f_.lastAt = toString(ts); f_.lastAssistantAt = toString(ts); }
    const json& msg = get(rec, "message");
    const json& model = get(msg, "model");
    if (model.is_string() && !str(model).empty()) f_.lastModel = str(model);
    const json& effort = get(rec, "effort");
    if (effort.is_string() && !str(effort).empty()) f_.lastEffort = str(effort);
    const json& u = get(msg, "usage");
    if (u.is_object() || u.is_array()) {
      auto n = [&](const char* k) { const json& v = get(u, k); return truthy(v) ? toNumber(v) : 0.0; };
      f_.lastContextTokens = n("input_tokens") + n("cache_creation_input_tokens") + n("cache_read_input_tokens");
    }
    const json& c = get(msg, "content");
    if (c.is_array()) {
      std::optional<std::string> last;
      for (const json& b : c) {
        if (!truthy(b) || get(b, "type") != "text" || !get(b, "text").is_string()) continue;
        const std::string tx = trim(str(get(b, "text")));
        if (!tx.empty()) last = tx;
      }
      if (last) { f_.lastText = last; f_.lastRole = "assistant"; }
    }
  }
  f_.title = customTitle ? customTitle : aiTitle;
}

}}  // namespace deck::session
