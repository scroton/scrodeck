// core/deck/cloud.cpp — see cloud.hpp. lib/cloud.js, line for line (plus main.js cloudSpec's two text rules).
#include "cloud.hpp"
#include "util.hpp"
#include "../session/jsutil.hpp"
#include <cmath>
#include <limits>

namespace deck { namespace core {

namespace js = deck::session::js;
const char* const kCloudApi = "https://api.anthropic.com";

namespace {

bool alnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
json stamp(double d) { return d == std::floor(d) && std::fabs(d) < 9e15 ? json((int64_t)d) : json(d); }
// a || b || c ... over json values
const json& orElse(const json& a, const json& b) { return js::truthy(a) ? a : b; }

// String(x) for any JSON value, as JavaScript prints it (arrays joined with ',', objects "[object Object]")
std::string jsString(const json& v) {
  if (v.is_array()) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) { if (i) out += ','; if (!v[i].is_null()) out += jsString(v[i]); }
    return out;
  }
  if (v.is_object()) return "[object Object]";
  return js::toString(v);
}
// Number(x) for a value read off an object: a missing key is undefined (NaN), an array or object goes through
// its text ([] is 0, [5] is 5, {} is NaN)
double jsNumber(const json& o, const char* key) {
  if (!o.is_object()) return std::numeric_limits<double>::quiet_NaN();
  const auto it = o.find(key);
  if (it == o.end()) return std::numeric_limits<double>::quiet_NaN();
  if (it->is_array() || it->is_object()) return js::toNumber(json(jsString(*it)));
  return js::toNumber(*it);
}
// view's ms(): Date.parse(v || ''), finite or null. DIFFERENCE: only a string in an ISO 8601 form is read
// (js::dateParse); V8's legacy parser also reads other spellings ("Oct 12 2026", a number's text) and those are
// null here. The API sends ISO.
json ms(const json& v) {
  if (!v.is_string()) return json();
  const double n = js::dateParse(v.get_ref<const std::string&>());
  return std::isfinite(n) ? stamp(n) : json();
}
// encodeURIComponent over UTF-8 bytes
std::string encodeURIComponent(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (alnum(c) || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
// cloudKey(v.id) for a stored view: its id is a string, or null (a row whose id was "")
std::string keyOf(const json& view) { const json& id = js::get(view, "id"); return id.is_string() ? cloudKey(id.get_ref<const std::string&>()) : std::string(); }
// (1234567).toLocaleString() as en-US prints it
std::string grouped(size_t n) {
  std::string d = std::to_string(n), out;
  for (size_t i = 0; i < d.size(); i++) { if (i && (d.size() - i) % 3 == 0) out += ','; out += d[i]; }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- ids
// cloudKey: String(id || '').replace(/^(session_|cse_)/, '')
std::string cloudKey(const std::string& id) {
  if (id.compare(0, 8, "session_") == 0) return id.substr(8);
  if (id.compare(0, 4, "cse_") == 0) return id.substr(4);
  return id;
}
// webId: id ? 'session_' + cloudKey(id) : null
std::string cloudWebId(const std::string& id) { return id.empty() ? std::string() : "session_" + cloudKey(id); }
// webUrl: `https://claude.ai/code/${webId(id)}` — a null webId prints as "null" in the JS; kept
std::string cloudWebUrl(const std::string& id) { return "https://claude.ai/code/" + (id.empty() ? std::string("null") : cloudWebId(id)); }
// isCloudId: /^(session_|cse_)[A-Za-z0-9]{8,}$/
bool isCloudId(const std::string& id) {
  size_t at;
  if (id.compare(0, 8, "session_") == 0) at = 8; else if (id.compare(0, 4, "cse_") == 0) at = 4; else return false;
  if (id.size() - at < 8) return false;
  for (size_t i = at; i < id.size(); i++) if (!alnum((unsigned char)id[i])) return false;
  return true;
}

// ---------------------------------------------------------------- parseCreated
json cloudParseCreated(const std::string& text) {
  // .replace(/\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g, '')   an OSC, ended by BEL or ESC \, is removed
  std::string a;
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == ']') {
      size_t j = i + 2;
      while (j < text.size() && text[j] != '\x07' && text[j] != '\x1b') j++;
      if (j < text.size() && text[j] == '\x07') { i = j + 1; continue; }
      if (j + 1 < text.size() && text[j] == '\x1b' && text[j + 1] == '\\') { i = j + 2; continue; }
    }
    a += text[i++];
  }
  // .replace(/\x1b\[[0-9;?<>=]*[A-Za-z]/g, '\n')         a CSI sequence becomes a line end
  std::string plain;
  for (size_t i = 0; i < a.size();) {
    if (a[i] == '\x1b' && i + 1 < a.size() && a[i + 1] == '[') {
      size_t j = i + 2;
      while (j < a.size() && ((a[j] >= '0' && a[j] <= '9') || a[j] == ';' || a[j] == '?' || a[j] == '<' || a[j] == '>' || a[j] == '=')) j++;
      if (j < a.size() && ((a[j] >= 'A' && a[j] <= 'Z') || (a[j] >= 'a' && a[j] <= 'z'))) { plain += '\n'; i = j + 1; continue; }
    }
    plain += a[i++];
  }
  // /claude\.ai\/code\/(session_[A-Za-z0-9]+)/
  static const std::string link = "claude.ai/code/session_";
  std::string id;
  for (size_t at = plain.find(link); at != std::string::npos; at = plain.find(link, at + 1)) {
    size_t j = at + link.size();
    while (j < plain.size() && alnum((unsigned char)plain[j])) j++;
    if (j > at + link.size()) { id = plain.substr(at + 15, j - at - 15); break; }
  }
  if (id.empty()) return json();
  // /Created cloud session:[ \t]*([^\r\n]*)/
  static const std::string created = "Created cloud session:";
  json title;
  const size_t c = plain.find(created);
  if (c != std::string::npos) {
    size_t s = c + created.size();
    while (s < plain.size() && (plain[s] == ' ' || plain[s] == '\t')) s++;
    size_t e = s;
    while (e < plain.size() && plain[e] != '\r' && plain[e] != '\n') e++;
    const std::string t = js::trim(plain.substr(s, e - s));
    if (!t.empty()) title = t;
  }
  return { { "id", id }, { "title", title } };
}

// ---------------------------------------------------------------- view, listable
json cloudView(const json& row) {
  if (!row.is_object() || !js::get(row, "id").is_string()) return json();
  static const json none = json::object();
  const json& emIn = js::get(row, "external_metadata");
  const json& em = (emIn.is_object() || emIn.is_array()) ? emIn : none;      // an array is an object with none of the keys
  const json& pts = orElse(js::get(em, "post_turn_summary"), orElse(js::get(row, "post_turn_summary"), none));
  const json& ctx = orElse(js::get(em, "context_usage"), none);
  const json& ws = js::get(row, "worker_status");
  const char* status = js::get(row, "status") == "archived" ? "archived" : ws == "running" ? "busy" : ws == "requires_action" ? "waiting" : "idle";
  const json& pa = js::get(em, "pending_action");
  const json& pending = (pa.is_object() || pa.is_array()) ? js::get(pa, "tool_name") : js::null();
  const std::string webId = cloudWebId(js::get(row, "id").get_ref<const std::string&>());
  json title;
  if (js::get(row, "title").is_string()) { const std::string t = js::trim(js::get(row, "title").get_ref<const std::string&>()); if (!t.empty()) title = t; }   // the TRIMMED text: (a && b) is b
  const json created = ms(js::get(row, "created_at"));
  json lastAt = ms(js::get(row, "last_event_at"));
  if (!js::truthy(lastAt)) lastAt = ms(js::get(row, "updated_at"));          // ||: a 0 falls through as a null does
  if (!js::truthy(lastAt)) lastAt = created;
  json context;
  const double maxT = jsNumber(ctx, "max_tokens"), used = jsNumber(ctx, "used_tokens");
  if (maxT > 0 && used >= 0) {                                                // NaN (a missing or unreadable value) compares false
    const double pct = std::fmin(100.0, js::round(100 * used / maxT));
    context = stamp(pct == 0 ? 0 : pct);
  }
  const bool waiting = std::string(status) == "waiting";
  return {
    { "id", webId.empty() ? json() : json(webId) },
    { "title", title },
    { "status", status },
    { "environmentKind", orElse(js::get(row, "environment_kind"), js::null()) },
    { "lastAt", lastAt },
    { "createdAt", created },
    { "summary", orElse(js::get(pts, "status_detail"), orElse(js::get(pts, "recent_action"), js::null())) },
    { "waitingFor", waiting ? orElse(js::get(pts, "needs_action"), orElse(pending, js::null())) : js::null() },
    { "model", orElse(js::get(em, "last_served_model"), orElse(js::truthy(js::get(row, "config")) ? js::get(js::get(row, "config"), "model") : js::null(), js::null())) },
    { "context", context },
    { "unread", js::truthy(js::get(row, "unread")) },
  };
}

// listable: !!v && v.status !== 'archived' && v.environmentKind !== 'bridge'
bool cloudListable(const json& view) {
  return js::truthy(view) && js::get(view, "status") != "archived" && js::get(view, "environmentKind") != "bridge";
}

// ---------------------------------------------------------------- main.js cloudSpec
// String(s.task || '').replace(/\r\n?/g, '\n').trim()
std::string cloudCleanTask(const json& task) {
  const std::string s = js::truthy(task) ? jsString(task) : std::string();
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\r') { out += '\n'; if (i + 1 < s.size() && s[i + 1] == '\n') i++; } else out += s[i];
  }
  return js::trim(out);
}
std::string cloudTaskError(const std::string& task) {
  if (task.empty()) return "a cloud session starts from a task: say what it should do";
  size_t units = 0;
  for (size_t i = 0; i < task.size();) units += js::nextCodePoint(task, i) >= 0x10000 ? 2 : 1;      // task.length
  if (units > (size_t)kCloudTaskMax)
    return "the task is " + grouped(units) + " characters; a command line takes at most " + grouped((size_t)kCloudTaskMax) + ". Put the detail in a file in the repo and point the task at it.";
  return std::string();
}

// ---------------------------------------------------------------- CloudPoller
struct CloudPoller::Pass {
  std::vector<CloudTile> tiles;
  size_t tile = 0, account = 0;
  double now = 0;
  bool changed = false;
  std::function<void(bool)> done;
};

CloudPoller::CloudPoller(Clock& clock, Http* http, Credentials* credentials)
  : clock_(clock), http_(http), creds_(credentials), alive_(std::make_shared<bool>(true)) {}
CloudPoller::~CloudPoller() { *alive_ = false; }

void CloudPoller::setAccounts(std::vector<CloudAccount> accounts) { accounts_ = std::move(accounts); }

// token(a)
std::string CloudPoller::token(const CloudAccount& a) {
  if (!creds_) return std::string();
  const json p = creds_->readOAuth(a.dir);
  const json& c = js::get(p, "claudeAiOauth");
  if (!js::truthy(c) || !js::truthy(js::get(c, "accessToken"))) return std::string();
  const json& exp = js::get(c, "expiresAt");
  if (js::truthy(exp) && js::toNumber(exp) < clock_.nowMs()) return std::string();   // a running session in that account refreshes it
  return js::toString(js::get(c, "accessToken"));
}

// get(a, url): `got(true, body, "")` or `got(false, null, the error's message)`. The clock is read here, per
// request, for the back-off and the token's expiry (the pass's own `now` is read once, in poll).
void CloudPoller::get(const CloudAccount& a, const std::string& url, Got got) {
  const auto b = backoffUntil_.find(a.id);
  if (b != backoffUntil_.end() && b->second > clock_.nowMs()) { got(false, json(), "rate limited"); return; }
  // with no Http seam nothing can be asked: read as no login (the JS always has a fetch)
  const std::string tok = http_ ? token(a) : std::string();
  if (tok.empty()) { got(false, json(), "not logged in, or the login has expired"); return; }
  const std::map<std::string, std::string> headers = {
    { "Authorization", "Bearer " + tok }, { "anthropic-beta", "oauth-2025-04-20" }, { "anthropic-version", "2023-06-01" } };
  const std::string id = a.id;
  const std::shared_ptr<bool> alive = alive_;
  http_->get(url, headers, [this, alive, id, got](const HttpResponse& res) {
    if (!*alive) return;
    if (!res.error.empty()) { got(false, json(), res.error); return; }       // String(e.message)
    if (res.status == 429) {
      double retry = 0;                                                        // Number(res.headers.get('retry-after')) * 1000
      for (const auto& h : res.headers) if (lowerAscii(h.first) == "retry-after") { const double v = js::toNumber(json(h.second)); retry = std::isnan(v) ? 0 : v * 1000; }
      backoffUntil_[id] = clock_.nowMs() + (retry > 0 ? retry : kCloudBackoffMs);
      got(false, json(), "rate limited"); return;
    }
    if (res.status < 200 || res.status > 299) { got(false, json(), "HTTP " + std::to_string(res.status)); return; }
    const json j = json::parse(res.body, nullptr, false);
    // DIFFERENCE: res.json() throws V8's own wording ("Unexpected token '<', ..."); usage.cpp's substitute is used
    if (j.is_discarded()) { got(false, json(), "Unexpected response (not JSON)"); return; }
    got(true, j, std::string());
  });
}

// session(id): id ? this.byKey.get(cloudKey(id)) || null : null
json CloudPoller::session(const std::string& id) const {
  if (id.empty()) return json();
  const auto it = byKey_.find(cloudKey(id));
  return it == byKey_.end() ? json() : it->second;
}
void CloudPoller::soon(const std::string& id) {
  if (id.empty()) return;
  const auto it = byKey_.find(cloudKey(id));
  if (it != byKey_.end()) it->second["fetchedAt"] = 0;
}
// store(v, account, now)
void CloudPoller::store(const json& view, const std::string& account, double now) {
  json v = view; v["account"] = account; v["fetchedAt"] = stamp(now); v["error"] = nullptr;
  byKey_[keyOf(view)] = v;
}

// poll(tiles, now = Date.now())
void CloudPoller::poll(const std::vector<CloudTile>& tiles, std::function<void(bool changed)> done) {
  if (busy_) { if (done) done(false); return; }
  busy_ = true;
  std::shared_ptr<Pass> pass = std::make_shared<Pass>();
  pass->tiles = tiles; pass->now = clock_.nowMs(); pass->done = std::move(done);
  next(pass);
}

// The body of poll's two loops, resumed after every answer: the next request that is due is made and this
// returns; its answer calls back in here. Nothing left: the pass ends.
void CloudPoller::next(std::shared_ptr<Pass> p) {
  const double now = p->now;
  while (p->tile < p->tiles.size()) {
    const CloudTile t = p->tiles[p->tile++];
    if (t.cloudId.empty()) continue;
    const CloudAccount* found = nullptr;
    for (const CloudAccount& x : accounts_) if (x.id == t.account) { found = &x; break; }
    if (!found) continue;
    const CloudAccount a = *found;
    const json cur = session(t.cloudId);
    if (!cur.is_null() && now - js::toNumber(js::get(cur, "fetchedAt")) < kCloudTileMs) continue;
    get(a, std::string(kCloudApi) + "/v1/code/sessions/" + encodeURIComponent(cloudWebId(t.cloudId)), [this, p, t, a, cur](bool ok, const json& j, const std::string& error) {
      if (ok) {
        // view((j && (j.response_shape || j.session)) || j)
        const json& inner = orElse(js::get(j, "response_shape"), js::get(j, "session"));
        const json v = cloudView(js::truthy(j) && js::truthy(inner) ? inner : j);
        if (!v.is_null()) { store(v, a.id, p->now); p->changed = true; }
      } else {
        json v = cur.is_null() ? json{ { "id", cloudWebId(t.cloudId) }, { "status", "idle" } } : cur;
        v["account"] = a.id; v["fetchedAt"] = stamp(p->now); v["error"] = error;
        byKey_[cloudKey(t.cloudId)] = v;
        p->changed = true;
      }
      next(p);
    });
    return;
  }
  while (p->account < accounts_.size()) {
    const CloudAccount a = accounts_[p->account++];
    const auto at = listAt_.find(a.id);
    if (now - (at == listAt_.end() ? 0 : at->second) < kCloudListMs) continue;
    listAt_[a.id] = now;
    get(a, std::string(kCloudApi) + "/v1/code/sessions?limit=100&statuses=active", [this, p, a](bool ok, const json& j, const std::string&) {
      if (ok) {
        json rows = json::array();
        const json& data = js::get(j, "data");
        if (data.is_array()) for (const json& row : data) { const json v = cloudView(row); if (cloudListable(v)) rows.push_back(v); }
        lists_[a.id] = rows;
        for (const json& v : rows) store(v, a.id, p->now);       // a list row carries the same fields as a single read
        p->changed = true;
      }                                                          // no access, logged out, rate limited: keep the last list
      next(p);
    });
    return;
  }
  busy_ = false;
  const std::function<void(bool)> done = std::move(p->done);
  if (done) done(p->changed);
}

// elsewhere(hosted): every account's live cloud sessions that `hosted` (cloudKeys) does not already hold
json CloudPoller::elsewhere(const std::set<std::string>& hostedKeys) const {
  json out = json::array();
  for (const CloudAccount& a : accounts_) {
    const auto it = lists_.find(a.id);
    if (it == lists_.end()) continue;
    for (const json& v : it->second) if (!hostedKeys.count(keyOf(v))) { json e = v; e["account"] = a.id; out.push_back(e); }
  }
  return out;
}

json CloudPoller::lists() const {
  json out = json::object();
  for (const auto& kv : lists_) out[kv.first] = kv.second;
  return out;
}

}}  // namespace deck::core
