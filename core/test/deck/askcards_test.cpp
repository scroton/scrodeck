// core/test/deck/askcards_test.cpp — core/deck/askcards (the native ask cards) against
// core/test/fixtures/deck/askcards.json, which core/test/deck/gen-askcards.js records: pages built by the REAL
// ask_page.py, and page.html's OWN script run on them headless for scripted clicks and typing. Here:
//   * the model: every card of the page, in its order, each named by the page's data; its context re-rendered the
//     way the skill's render_md writes it equals the page's own HTML (so nothing of it was lost); the reply buttons
//     in the page's order, the off ones off; the older skill's page is `legacy`;
//   * the state: after every step, S (the page's `change` message, key order and all), its counts, every card's
//     state chip / answered / later / checked buttons / box text equal the page's;
//   * answers() / asText() at the end equal the page's, and JSON.stringify(answers, null, 1) - the answers file -
//     equals the page's byte for byte.
// `askcards-emit <page.html> <steps.json> <out.json>` (deck_test.cpp main): the scripted answers of a page, for
// core/test/deck/askcards-read.js (the skill's `read` must take them).
#include "t.hpp"
#include "../../deck/askcards.hpp"

#include <fstream>
#include <regex>
#include <sstream>

void testAskCards(const std::string& fixtures);
int askCardsEmit(const std::string& page, const std::string& steps, const std::string& out);

namespace {

using namespace deck::core::askcards;

std::string readAll(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
std::string esc(const std::string& s, bool quote) {
  std::string o;
  for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (quote && c == '"') o += "&quot;"; else o += c; }
  return o;
}
// a line as the skill's inline() writes it: <b> outside <a> outside <code>
std::string lineHtml(const Line& l) {
  std::string o;
  for (auto& r : l) {
    std::string t = esc(r.text, false);
    if (r.code) t = "<code>" + t + "</code>";
    if (!r.href.empty()) t = "<a href=\"" + esc(r.href, true) + "\" target=\"_blank\" rel=\"noopener\">" + t + "</a>";
    if (r.bold) t = "<b>" + t + "</b>";
    o += t;
  }
  return o;
}
std::string joinLines(const std::vector<Line>& ls, const char* sep) { std::string o; for (size_t i = 0; i < ls.size(); i++) { if (i) o += sep; o += lineHtml(ls[i]); } return o; }
// the blocks as render_md writes them
std::string blocksHtml(const std::vector<Block>& bs) {
  std::string o;
  for (size_t i = 0; i < bs.size(); i++) {
    const Block& b = bs[i];
    if (i) o += "\n";
    switch (b.kind) {
      case Block::Para: o += "<p>" + joinLines(b.lines, "<br>") + "</p>"; break;
      case Block::Quote: o += "<blockquote>" + joinLines(b.lines, "<br>") + "</blockquote>"; break;
      case Block::Bullets: o += "<ul>"; for (auto& l : b.lines) o += "<li>" + lineHtml(l) + "</li>"; o += "</ul>"; break;
      case Block::Code: o += "<pre>" + esc(b.code, true) + "</pre>"; break;
      case Block::Table:
        o += "<div class=\"tablewrap\"><table><thead><tr>";
        for (auto& h : b.head) o += "<th>" + lineHtml(h) + "</th>";
        o += "</tr></thead><tbody>";
        for (size_t r = 0; r < b.rows.size(); r++) {
          o += "<tr>";
          for (size_t c = 0; c < b.rows[r].size(); c++) o += std::string(b.num[r][c] ? "<td class=num>" : "<td>") + lineHtml(b.rows[r][c]) + "</td>";
          o += "</tr>";
        }
        o += "</tbody></table></div>";
        break;
    }
  }
  return o;
}
// what the browser shows the same: the quote entities as their characters, white space collapsed (outside <pre>),
// none next to a block's own tags or a <br>
std::string norm(std::string s) {
  for (size_t at = 0; (at = s.find("\r\n", at)) != std::string::npos;) s.erase(at, 1);
  for (auto pr : { std::make_pair(std::string("&quot;"), std::string("\"")), std::make_pair(std::string("&#x27;"), std::string("'")) }) {
    for (size_t at = 0; (at = s.find(pr.first, at)) != std::string::npos;) { s.replace(at, pr.first.size(), pr.second); at += pr.second.size(); }
  }
  std::string o; bool pre = false, sp = false;
  for (size_t i = 0; i < s.size(); i++) {
    if (s.compare(i, 5, "<pre>") == 0) pre = true;
    if (s.compare(i, 6, "</pre>") == 0) pre = false;
    const char c = s[i];
    if (!pre && (c == ' ' || c == '\n' || c == '\t' || c == '\r')) { if (!sp) o += ' '; sp = true; continue; }
    sp = false; o += c;
  }
  static const std::regex around(R"( ?(</?(p|li|ul|blockquote|th|td|tr|thead|tbody|table|div)( class=num| class="tablewrap")?>|<br>) ?)");
  return std::regex_replace(o, around, "$1");
}
// each card's context HTML in the page: between <div class="context"> and the options / the replies
std::map<std::string, std::string> contextsOf(const std::string& html) {
  std::map<std::string, std::string> out;
  for (size_t a = 0; (a = html.find("<article class=\"", a)) != std::string::npos; a++) {
    const size_t q = html.find("data-qid=\"", a); if (q == std::string::npos) break;
    const std::string qid = html.substr(q + 10, html.find('"', q + 10) - q - 10);
    const size_t c = html.find("<div class=\"context\">", a), r = html.find("<div class=\"replies\">", a);
    if (c == std::string::npos || r == std::string::npos) continue;
    size_t e = html.find("<div class=\"options\"", c); if (e == std::string::npos || e > r) e = r;
    std::string inner = html.substr(c + 21, e - c - 21);
    while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\n' || inner.back() == '\r')) inner.pop_back();
    if (inner.size() >= 6 && inner.compare(inner.size() - 6, 6, "</div>") == 0) inner.resize(inner.size() - 6);
    out[qid] = inner;
  }
  return out;
}
OJ cardsView(const Page& p, const State& st) {
  OJ v = OJ::array();
  for (auto& c : p.cards) {
    OJ checked = OJ::array();
    for (auto& o : c.options) if (st.checked(c.qid, o.key)) checked.push_back(o.key);
    for (auto& r : c.replies) if (st.checked(c.qid, r.pick)) checked.push_back(r.pick);
    v.push_back({ { "qid", c.qid }, { "label", st.stateLabel(c.qid) }, { "answered", st.answered(c.qid) }, { "later", st.later(c.qid) },
                  { "checked", checked }, { "text", st.text(c.qid) }, { "readOnly", st.locked() } });
  }
  return v;
}
OJ changeOf(const State& st) { return st.progressJson(true); }
bool apply(State& st, const Page& p, const OJ& step) {
  const std::string d = step.value("do", std::string());
  if (d == "pick") return st.pick(step.value("qid", std::string()), step.value("k", std::string()));
  if (d == "type") return st.type(step.value("qid", std::string()), step.value("text", std::string()));
  if (d == "clear") return st.clear();
  (void)p;
  return false;
}
std::string show(const OJ& v) { return stringify(v).substr(0, 700); }
void sameOJ(const std::string& name, const OJ& got, const OJ& want) { const bool ok = got == want; check(name, ok, ok ? "" : "got  " + show(got) + "\n      want " + show(want)); }

}  // namespace

void testAskCards(const std::string& fixtures) {
  if (!want("askcards")) return;
  const OJ fx = OJ::parse(readAll(fixtures + "/askcards.json"), nullptr, false);
  check("askcards: the fixture loads", fx.is_object() && fx.contains("pages"));
  if (!fx.is_object()) return;
  const std::string savedAt = fx.value("savedAt", std::string());
  check("askcards: isoTime is Date.toISOString", isoTime(1795453200000LL) == savedAt, isoTime(1795453200000LL));
  check("askcards: isoTime before 1970 and a leap day", isoTime(-1) == "1969-12-31T23:59:59.999Z" && isoTime(951782400123LL) == "2000-02-29T00:00:00.123Z", isoTime(-1));
  for (auto& pg : fx["pages"]) {
    const std::string name = pg.value("name", std::string()), html = pg.value("html", std::string());
    const Page p = parsePage(html);
    const std::string at = "askcards " + name + ": ";
    if (html.find("id=\"askdata\"") == std::string::npos) {
      check(at + "an older skill's page (no askdata) is legacy, not ok", p.legacy && !p.ok, p.error);
      continue;
    }
    check(at + "parses", p.ok && !p.legacy, p.error);
    check(at + "its id is the page's", p.id == pg.value("id", std::string()), p.id);
    size_t arts = 0; for (size_t a = 0; (a = html.find("<article class=\"card", a)) != std::string::npos; a++) arts++;
    check(at + "a card for every <article>", p.cards.size() == arts && arts > 0, std::to_string(p.cards.size()) + " of " + std::to_string(arts));
    bool named = true, keys = true; std::string why;
    for (auto& c : p.cards) {
      if (c.item < 0) { named = false; why += c.qid + " "; continue; }
      const Item& it = p.items[c.item];
      std::vector<std::string> a, b; for (auto& o : c.options) a.push_back(o.key); for (auto& o : it.options) b.push_back(o.first);
      if (a != b) { keys = false; why += c.qid + " options "; }
      if (c.title != it.title) { keys = false; why += c.qid + " title '" + c.title + "' "; }
      if (c.replies.empty() || c.replies.back().pick != "r:none" || !c.replies.back().none) { keys = false; why += c.qid + " replies "; }
      if (c.placeholder != "Your answer to " + c.qid) { keys = false; why += c.qid + " placeholder "; }
    }
    check(at + "every card is one of the page's items", named, why);
    check(at + "options, titles, reply rows and boxes as the page has them", keys, why);
    // the context, re-rendered as render_md writes it, is the page's own
    const auto ctx = contextsOf(html);
    std::string lost;
    for (auto& c : p.cards) { auto it = ctx.find(c.qid); if (it == ctx.end()) continue; const std::string g = norm(blocksHtml(c.context)), w = norm(it->second); if (g != w) lost += "\n      " + c.qid + ":\n      got  " + g.substr(0, 400) + "\n      want " + w.substr(0, 400); }
    check(at + "every card's context, drawn back as HTML, is the page's (nothing lost)", lost.empty(), lost);
    if (name == "rich") {
      const Card& c1 = p.cards[0];
      check(at + "the reply buttons follow the page's order (settings reply_order), See below last",
            c1.replies.size() == 10 && c1.replies[0].pick == "r:later" && c1.replies[1].pick == "r:yourcall" && c1.replies[2].pick == "r:explain" && c1.replies[3].pick == "r:check" && c1.replies[4].pick == "r:all",
            c1.replies.empty() ? "" : c1.replies[0].pick);
      check(at + "multi: pick any; the block kinds (p, ul, quote)", c1.pickAny && c1.context.size() == 3 && c1.context[0].kind == Block::Para && c1.context[1].kind == Block::Bullets && c1.context[2].kind == Block::Quote && c1.context[2].lines.size() == 2);
      check(at + "the bold run and the code run", c1.context[0].lines[0].size() == 3 && c1.context[0].lines[0][1].bold && c1.context[0].lines[0][1].text == "Saturday" && c1.context[1].lines[1].back().code);
      check(at + "a consequence keeps its bold, its ' - ' is not part of it", lineText(c1.options[2].cons) == "the big one" && c1.options[2].cons.size() == 3 && c1.options[2].cons[1].bold && c1.options[1].cons.empty());
      const int g1 = p.cardOf("Q2a"), g2 = p.cardOf("Q2b");
      check(at + "a group: the first explains, the next one points back", g1 >= 0 && g2 >= 0 && p.cards[g1].groupFirst && p.cards[g2].groupNext && p.cards[g2].groupLast
            && p.cards[g1].chips.size() == 1 && p.cards[g1].chips[0] == "explains Q2a-Q2b" && p.cards[g2].chips[0] == "see Q2a above");
      check(at + "a code block keeps its lines", g1 >= 0 && p.cards[g1].context.size() == 2 && p.cards[g1].context[1].kind == Block::Code && p.cards[g1].context[1].code == "if job: DEV\n  else: QA");
      const int t = p.cardOf("Q3");
      check(at + "a table: the header, the rows, the number cells", t >= 0 && p.cards[t].context.size() == 1 && p.cards[t].context[0].kind == Block::Table && p.cards[t].context[0].head.size() == 3
            && p.cards[t].context[0].rows.size() == 3 && p.cards[t].context[0].num[0][1] && p.cards[t].context[0].num[0][2] && !p.cards[t].context[0].num[0][0] && !p.cards[t].context[0].num[1][2]);
      const int one = p.cardOf("Q4");
      bool bothOff = false; if (one >= 0) for (auto& r : p.cards[one].replies) if (r.pick == "r:both") bothOff = r.off && r.tip == "Only on a card with two or more options";
      check(at + "one option: Both is there but off (its slot kept)", bothOff);
      check(at + "a bare URL and a bare path are links", one >= 0 && p.cards[one].context[0].lines[0].size() >= 4 && p.cards[one].context[0].lines[0][1].href == "http://example.invalid/po/1"
            && p.cards[one].context[0].lines[0][3].href.rfind("file:///", 0) == 0);
      check(at + "the question's bold", one >= 0 && p.cards[one].question.size() == 3 && p.cards[one].question[1].bold);
      check(at + "the summary and the FYI", p.summary.size() == 2 && p.fyi.size() == 1 && p.summary[0].size() >= 6 && p.summary[0][3].code && !p.summary[0][5].href.empty(), stringify(pageJson(p)["summary"]));
      check(at + "the recommended option", p.cards[g1].options[0].rec && !p.cards[g1].options[1].rec);
    }
    if (name == "round2") {
      int carried = 0, from = 0; for (auto& c : p.cards) { if (c.carried) carried++; for (auto& ch : c.chips) if (ch == "from round 1") from++; }
      check(at + "the carried cards: under the subhead, each from round 1", carried == 7 && from == 7 && !p.cards[0].carried && p.cards[2].carried, std::to_string(carried));
    }
    const OJ model = pageJson(p);
    check(at + "pageJson names every card", model["cards"].size() == p.cards.size() && model["items"].size() == p.items.size());
    // ---- the scenarios: page.html's own script, step by step
    for (auto& sc : pg["scenarios"]) {
      const std::string sn = at + sc.value("name", std::string()) + ": ";
      State st(&p);
      const OJ& rd = sc["ready"];
      const State::Progress g0 = st.progress();
      check(sn + "the counts in 'ready'", g0.answered == rd.value("answered", -1) && g0.later == rd.value("later", -1) && g0.total == rd.value("total", -1) && g0.drafted == rd.value("drafted", -1));
      const OJ& init = sc["init"];
      const OJ initState = init.contains("state") ? init["state"] : OJ();
      st.load(init.contains("state") ? &initState : nullptr, init.value("readonly", false));
      const OJ& loaded = sc["loaded"];
      OJ want = loaded["change"].is_null() ? OJ() : loaded["change"];
      if (!want.is_null()) sameOJ(sn + "after load: S and its counts", changeOf(st), want);
      sameOJ(sn + "after load: the cards", cardsView(p, st), loaded["cards"]);
      OJ lastState = want.is_null() ? changeOf(st) : want;
      for (size_t i = 0; i < sc["steps"].size(); i++) {
        const OJ& step = sc["steps"][i];
        const bool did = apply(st, p, step);
        const OJ& after = sc["after"][i];
        const std::string sn2 = sn + "step " + std::to_string(i + 1) + " " + stringify(step) + ": ";
        if (!after["change"].is_null()) { sameOJ(sn2 + "S and its counts", changeOf(st), after["change"]); lastState = after["change"]; check(sn2 + "it did something", did); }
        else { sameOJ(sn2 + "nothing changed", changeOf(st), lastState); check(sn2 + "it did nothing (the page sent no change)", !did); }
        sameOJ(sn2 + "the cards", cardsView(p, st), after["cards"]);
      }
      const OJ A = st.answers(savedAt);
      sameOJ(sn + "answers()", A, sc["answers"]);
      check(sn + "asText()", State::asText(A) == sc.value("text", std::string()), State::asText(A));
      check(sn + "the answers file (JSON.stringify(answers, null, 1)) byte for byte", stringify(A, 1) == sc.value("file", std::string()), stringify(A, 1).substr(0, 300));
      check(sn + "locked as the page", st.locked() == sc.value("locked", false));
    }
  }
  // the state is left alone by things that are not buttons of the card
  {
    const Page p = parsePage(fx["pages"][0].value("html", std::string()));
    State st(&p);
    check("askcards: no such card / button: nothing happens", !st.pick("Q99", "a") && !st.pick("Q1", "z") && !st.pick("Q1", "r:nope") && st.state().empty());
    bool focus = false;
    check("askcards: See below asks for the box", st.pick("Q1", "r:none", &focus) && focus);
    st.lock();
    check("askcards: locked: no pick, no typing", !st.pick("Q2", "a") && !st.type("Q2", "x") && !st.clear());
    const OJ older = OJ::parse(R"({"Q1":{"later":true},"Q2":"junk","Q3":{"text":7,"pick":["a"]}})");
    State s2(&p); s2.load(&older, false);
    check("askcards: an older page's later flag; junk entries are nothing", s2.status("Q1") == "later" && s2.status("Q2") == "skipped" && s2.text("Q3").empty() && s2.status("Q3") == "answered");
  }
}

// deck-test <fixtures> <repo> askcards-emit <page.html> <steps.json> <out.json>: the scripted answers of a page
int askCardsEmit(const std::string& page, const std::string& steps, const std::string& out) {
  const Page p = parsePage(readAll(page));
  if (!p.ok) { std::printf("askcards-emit: %s: %s\n", page.c_str(), p.error.c_str()); return 2; }
  const OJ sc = OJ::parse(readAll(steps), nullptr, false);
  State st(&p);
  if (sc.is_object() && sc.contains("init")) { const OJ s = sc["init"].contains("state") ? sc["init"]["state"] : OJ(); st.load(&s, sc["init"].value("readonly", false)); }
  if (sc.is_object() && sc.contains("steps")) for (auto& step : sc["steps"]) apply(st, p, step);
  std::ofstream f(out, std::ios::binary | std::ios::trunc);
  f << stringify(st.answers(isoTime(1795453200000LL)), 1);
  std::printf("askcards-emit: %s -> %s (%s)\n", p.id.c_str(), out.c_str(), st.countText().c_str());
  return f ? 0 : 1;
}
