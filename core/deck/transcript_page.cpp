// core/deck/transcript_page.cpp — see transcript_page.hpp. lib/transcript-page.js, function for function; its
// regular expressions are written out by hand.
#include "transcript_page.hpp"
#include "../session/jsutil.hpp"
#include "../session/replay.hpp"
#include <algorithm>
#include <map>
#include <vector>

namespace deck { namespace core {

namespace js = deck::session::js;
using ojson = nlohmann::ordered_json;            // a JavaScript object keeps its keys in the order they were written
static const size_t npos = std::string::npos;
static const size_t BODY_MAX = 4000;

static const ojson& oget(const ojson& o, const char* k) { static const ojson none; if (!o.is_object()) return none; auto it = o.find(k); return it == o.end() ? none : *it; }
static bool truthy(const ojson& v) { return js::truthy(json(v)); }
static std::string toStr(const ojson& v) { return js::toString(json(v)); }
static std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  for (size_t p = s.find(from); p != npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
  return s;
}
static std::string dropCr(const std::string& s) { std::string o; for (char c : s) if (c != '\r') o += c; return o; }
static bool starts(const std::string& s, const char* p) { return js::startsWith(s, p); }

static std::vector<ojson> blocks(const ojson& content) {
  std::vector<ojson> out;
  if (content.is_string()) { out.push_back({ { "type", "text" }, { "text", content } }); return out; }
  if (content.is_array()) for (const ojson& b : content) if (b.is_object() || b.is_array()) out.push_back(b);
  return out;
}
static std::string textOf(const ojson& content) {
  std::string out; bool first = true;
  for (const ojson& b : blocks(content)) {
    if (oget(b, "type") != "text" || !truthy(oget(b, "text"))) continue;
    if (!first) out += "\n\n";
    first = false; out += toStr(oget(b, "text"));
  }
  return out;
}
// "Bash", "mcp__slack__slack_search_users" -> "Bash", "slack"
static std::string toolLabel(const ojson& name) {
  const std::string n = truthy(name) ? toStr(name) : std::string("tool");
  if (starts(n, "mcp__")) { const size_t e = n.find('_', 5); if (e != npos && e > 5 && n.compare(e, 2, "__") == 0) return n.substr(5, e - 5); }
  return n;
}
static std::string first(const ojson& v) {         // String(v || '').split(/\r?\n/)[0].trim().slice(0, 120)
  const std::string s = truthy(v) ? toStr(v) : std::string();
  size_t e = s.find('\n'); if (e == npos) e = s.size();
  std::string line = s.substr(0, e);
  if (e < s.size() && !line.empty() && line.back() == '\r') line.pop_back();
  return js::sliceUnits(js::trim(line), 120);
}
static std::string briefOf(const ojson& b) {
  const ojson& inp = oget(b, "input");
  if (!inp.is_object()) return std::string();
  if (truthy(oget(inp, "command"))) return first(oget(inp, "command"));
  if (truthy(oget(inp, "file_path"))) return first(oget(inp, "file_path"));
  if (truthy(oget(inp, "pattern"))) return first(oget(inp, "pattern")) + (truthy(oget(inp, "path")) ? "  in " + first(oget(inp, "path")) : std::string());
  if (truthy(oget(inp, "description"))) return first(oget(inp, "description"));
  if (truthy(oget(inp, "prompt"))) return first(oget(inp, "prompt"));
  if (truthy(oget(inp, "url"))) return first(oget(inp, "url"));
  for (auto it = inp.begin(); it != inp.end(); ++it) if (it.value().is_string() && !js::trim(it.value().get<std::string>()).empty()) return first(it.value());
  return std::string();
}
static std::string capped(const std::string& out) {
  const size_t len = deck::session::utf16Length(out);
  return len > BODY_MAX ? js::sliceUnits(out, BODY_MAX) + "\n\xE2\x80\xA6 (" + std::to_string(len - BODY_MAX) + " more characters)" : out;
}
static std::string bodyOf(const ojson& b) {
  const ojson& inp = oget(b, "input");
  std::string out;
  if (oget(inp, "content").is_string()) out = oget(inp, "content").get<std::string>();
  else if (oget(inp, "new_string").is_string()) out = (oget(inp, "old_string").is_string() ? "--- old\n" + oget(inp, "old_string").get<std::string>() + "\n+++ new\n" : std::string()) + oget(inp, "new_string").get<std::string>();
  else if (oget(inp, "command").is_string()) out = oget(inp, "command").get<std::string>();
  else if (oget(inp, "prompt").is_string()) out = oget(inp, "prompt").get<std::string>();
  else if (oget(inp, "new_source").is_string()) out = oget(inp, "new_source").get<std::string>();
  return capped(out);
}
static std::string resultText(const ojson& block) {
  const ojson& c = oget(block, "content");
  if (c.is_string()) return c.get<std::string>();
  std::string out; bool firstOne = true;
  if (c.is_array()) for (const ojson& x : c) {
    if (!x.is_object() || oget(x, "type") != "text" || !truthy(oget(x, "text"))) continue;
    if (!firstOne) out += "\n";
    firstOne = false; out += toStr(oget(x, "text"));
  }
  return out;
}

// What kind of machinery a 'user' record is, so it can be kept — folded under a label, never as you.
static std::string machineryLabel(const ojson& rec, const std::string& text, bool hasToolResult) {
  const std::string t = js::trim(text);
  if (t.find("<system-reminder>") != npos) return "system reminder";
  if (starts(t, "<bash-stdout>") || starts(t, "<bash-stderr>")) return "command output";
  if (truthy(oget(rec, "isCompactSummary")) || truthy(oget(rec, "isVisibleInTranscriptOnly"))) return "summary";
  // /<command-name>\s*([^<]+?)\s*<\/command-name>/
  for (size_t p = t.find("<command-name>"); p != npos; p = t.find("<command-name>", p + 1)) {
    const size_t a = p + 14, q = t.find('<', a);
    if (q != npos && q > a && t.compare(q, 15, "</command-name>") == 0) return "command " + js::trim(t.substr(a, q - a));
  }
  if (starts(t, "<local-command-stdout>")) return "command output";
  if (starts(t, "[Request interrupted")) return "interrupted";
  if (starts(t, "Caveat: The messages below")) return "local commands";
  if (truthy(oget(rec, "isMeta"))) return "context";
  // /^Base directory for this skill:\s*(.+)$/m — the body of a skill is posted as a 'user' record after the Skill call
  { const std::string mark = "Base directory for this skill:";
    for (size_t p = t.find(mark); p != npos; p = t.find(mark, p + 1)) {
      if (p != 0 && t[p - 1] != '\n' && t[p - 1] != '\r') continue;
      size_t i = p + mark.size();
      while (i < t.size()) { size_t j = i; if (!js::isSpace(js::nextCodePoint(t, j))) break; i = j; }
      size_t e = i; while (e < t.size() && t[e] != '\n' && t[e] != '\r') e++;
      if (e == i) continue;
      const std::string dir = js::trim(t.substr(i, e - i));
      const size_t slash = dir.find_last_of("\\/");
      return "skill " + (slash == npos ? dir : dir.substr(slash + 1));
    } }
  if (truthy(oget(rec, "toolUseResult")) || hasToolResult) return "tool result";
  return std::string();
}
// remove every <open>[^<]*</close>
static std::string dropElement(const std::string& s, const std::string& open, const std::string& close) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    if (s.compare(i, open.size(), open) == 0) { const size_t q = s.find('<', i + open.size()); if (q != npos && s.compare(q, close.size(), close) == 0) { i = q + close.size(); continue; } }
    out += s[i++];
  }
  return out;
}
static std::string machineryBody(const std::string& text) {
  std::string s = text;
  for (const char* tag : { "<bash-stdout>", "</bash-stdout>", "<bash-stderr>", "</bash-stderr>" }) s = replaceAll(s, tag, "\n");
  s = replaceAll(s, "<artifact-content-authored-by-others/>", "");
  s = replaceAll(s, "<system-reminder>", ""); s = replaceAll(s, "</system-reminder>", "");
  s = dropElement(s, "<command-name>", "</command-name>");
  for (const char* tag : { "<command-message>", "</command-message>", "<command-args>", "</command-args>", "<local-command-stdout>", "</local-command-stdout>" }) s = replaceAll(s, tag, "");
  std::string out; int run = 0;                    // \n{3,} -> \n\n
  for (char c : s) { if (c == '\n') { if (++run <= 2) out += c; } else { run = 0; out += c; } }
  return js::trim(out);
}
// What you typed, as you typed it: Claude Code's own wrapping tags taken off
static std::string userText(const std::string& text) {
  std::string t;
  auto tagAt = [&](const std::string& s, size_t i, const char* open) -> size_t {      // <pasted_content\b[^>]*> at i: its end, or npos
    const size_t n = std::char_traits<char>::length(open);
    if (s.compare(i, n, open) != 0) return npos;
    if (i + n < s.size() && js::isWordChar((unsigned char)s[i + n])) return npos;
    const size_t e = s.find('>', i + n);
    return e == npos ? npos : e + 1;
  };
  for (size_t i = 0; i < text.size();) {            // <pasted_content\b[^>]*>\n?
    const size_t e = tagAt(text, i, "<pasted_content");
    if (e != npos) { i = e; if (i < text.size() && text[i] == '\n') i++; continue; }
    t += text[i++];
  }
  std::string u;
  for (size_t i = 0; i < t.size();) {               // \n?<\/pasted_content\b[^>]*>
    const size_t at = t[i] == '\n' ? i + 1 : i;
    const size_t e = at < t.size() ? tagAt(t, at, "</pasted_content") : npos;
    if (e != npos) { i = e; continue; }
    u += t[i++];
  }
  std::string v;
  for (size_t i = 0; i < u.size();) {               // <bash-input>([\s\S]*?)<\/bash-input> -> '! ' + cmd.trim()
    if (u.compare(i, 12, "<bash-input>") == 0) { const size_t q = u.find("</bash-input>", i + 12); if (q != npos) { v += "! " + js::trim(u.substr(i + 12, q - i - 12)); i = q + 13; continue; } }
    v += u[i++];
  }
  size_t a = 0, b = v.size();
  while (a < b && v[a] == '\n') a++;
  while (b > a && v[b - 1] == '\n') b--;
  return v.substr(a, b - a);
}

json transcriptEntries(const std::string& raw) {
  json entries = json::array();
  struct Call { ojson id; std::string name, brief, body; };
  std::vector<Call> pending;
  std::map<std::string, std::string> results;
  auto flushTools = [&](const json& at) {
    if (pending.empty()) return;
    json calls = json::array();
    std::vector<std::pair<std::string, int>> counts;
    for (const Call& c : pending) {
      json o = { { "name", c.name }, { "brief", c.brief }, { "body", c.body } };
      if (truthy(c.id) && c.id.is_string() && results.count(c.id.get<std::string>())) o["result"] = capped(results[c.id.get<std::string>()]);
      calls.push_back(o);
      bool found = false; for (auto& kv : counts) if (kv.first == c.name) { kv.second++; found = true; }
      if (!found) counts.emplace_back(c.name, 1);
    }
    std::string text = "Called ";
    for (size_t i = 0; i < counts.size(); i++) { if (i) text += ", "; text += counts[i].second > 1 ? counts[i].first + " " + std::to_string(counts[i].second) + " times" : counts[i].first; }
    entries.push_back({ { "role", "tool" }, { "at", at }, { "text", text }, { "calls", calls } });
    pending.clear();
  };
  size_t a = 0;
  for (;;) {
    const size_t nl = raw.find('\n', a);
    const std::string line = raw.substr(a, nl == npos ? npos : nl - a);
    const bool last = nl == npos;
    a = nl + 1;
    if (line.size() >= 2) {
      const ojson rec = ojson::parse(line, nullptr, false);
      const std::string type = oget(rec, "type").is_string() ? oget(rec, "type").get<std::string>() : std::string();
      if (rec.is_object() && !truthy(oget(rec, "isSidechain")) && (type == "user" || type == "assistant")) {
        const ojson& content = oget(oget(rec, "message"), "content");
        const std::vector<ojson> bs = blocks(content);
        const json at = truthy(oget(rec, "timestamp")) ? json(oget(rec, "timestamp")) : json();
        if (type == "user") {
          // a "user" record carrying tool_result blocks is the machinery posting results back, not you
          bool all = !bs.empty(), any = false;
          for (const ojson& b : bs) {
            const bool tr = oget(b, "type") == "tool_result";
            if (tr && truthy(oget(b, "tool_use_id"))) results[toStr(oget(b, "tool_use_id"))] = resultText(b);
            if (tr) any = true; else all = false;
          }
          if (!all) {
            const std::string text = dropCr(textOf(content));
            if (!js::trim(text).empty()) {
              flushTools(at);
              const std::string label = machineryLabel(rec, text, any);
              if (!label.empty()) entries.push_back({ { "role", "meta" }, { "at", at }, { "label", label }, { "text", machineryBody(text) } });
              else entries.push_back({ { "role", "user" }, { "at", at }, { "text", userText(text) } });
            }
          }
        } else {
          const std::string text = textOf(content);
          if (!js::trim(text).empty()) {
            flushTools(at);
            const ojson& model = oget(oget(rec, "message"), "model");
            entries.push_back({ { "role", "assistant" }, { "at", at }, { "model", truthy(model) ? json(model) : json() }, { "effort", truthy(oget(rec, "effort")) ? json(oget(rec, "effort")) : json() }, { "text", dropCr(text) } });
          }
          // tool calls accumulate; they are summarised when the conversation next speaks
          for (const ojson& b : bs) if (oget(b, "type") == "tool_use") pending.push_back({ truthy(oget(b, "id")) ? oget(b, "id") : ojson(), toolLabel(oget(b, "name")), briefOf(b), bodyOf(b) });
        }
      }
    }
    if (last) break;
  }
  flushTools(entries.empty() ? json() : entries.back()["at"]);
  for (size_t i = 0; i < entries.size(); i++) entries[i]["i"] = i;
  return entries;
}

json transcriptPage(const std::string& jsonl, const json& opts) {
  const json all = transcriptEntries(jsonl);
  const double total = (double)all.size();
  const json& before = js::get(opts, "before");
  const json& lim = js::get(opts, "limit");
  const double end = before.is_null() ? total : std::max(0.0, std::min(total, js::toNumber(before)));
  const double limit = opts.is_object() && opts.contains("limit") ? js::toNumber(lim) : 30;      // (an explicit null is 0, as in JS)
  const double start = std::max(0.0, end - std::max(1.0, std::min(200.0, limit)));
  json entries = json::array();
  for (size_t i = (size_t)start; i < (size_t)end && i < all.size(); i++) entries.push_back(all[i]);
  return { { "total", (int)total }, { "from", (int)start }, { "to", (int)end }, { "entries", entries } };
}

}}  // namespace deck::core
