// core/deck/askcards.cpp — see askcards.hpp. The page's script is the skill's templates/page.html; every function
// below that carries a page.html name is that function, on the same strings.
#include "askcards.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>

namespace deck { namespace core { namespace askcards {

namespace {

// ---------------------------------------------------------------- strings as the JS has them (UTF-16)
std::u16string u16(const std::string& s) {
  std::u16string o; o.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    uint32_t cp = 0xFFFD; size_t n = 1;
    if (c < 0x80) cp = c;
    else if ((c >> 5) == 6 && i + 1 < s.size() && ((unsigned char)s[i + 1] >> 6) == 2) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); n = 2; if (cp < 0x80) cp = 0xFFFD; }
    else if ((c >> 4) == 14 && i + 2 < s.size() && ((unsigned char)s[i + 1] >> 6) == 2 && ((unsigned char)s[i + 2] >> 6) == 2) {
      cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); n = 3; if (cp < 0x800) cp = 0xFFFD;
    } else if ((c >> 3) == 30 && i + 3 < s.size() && ((unsigned char)s[i + 1] >> 6) == 2 && ((unsigned char)s[i + 2] >> 6) == 2 && ((unsigned char)s[i + 3] >> 6) == 2) {
      cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); n = 4; if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
    }
    if (cp >= 0x10000) { cp -= 0x10000; o.push_back((char16_t)(0xD800 + (cp >> 10))); o.push_back((char16_t)(0xDC00 + (cp & 0x3FF))); }
    else o.push_back((char16_t)cp);
    i += n;
  }
  return o;
}
std::string u8(const std::u16string& s) {
  std::string o; o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
    else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;   // a lone surrogate (UTF-8 cannot hold it)
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
  }
  return o;
}
// the JS's \s (WhiteSpace + LineTerminator), which trim() also removes
bool jsSpace(char16_t c) {
  return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
         c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}
std::u16string jsTrim(const std::u16string& s) {
  size_t a = 0, b = s.size();
  while (a < b && jsSpace(s[a])) a++;
  while (b > a && jsSpace(s[b - 1])) b--;
  return s.substr(a, b - a);
}
std::u16string trimStart(const std::u16string& s) { size_t a = 0; while (a < s.size() && jsSpace(s[a])) a++; return s.substr(a); }   // .replace(/^\s+/, '')
char16_t lowerCh(char16_t c) {
  if (c < 0x80) return c >= 'A' && c <= 'Z' ? (char16_t)(c + 32) : c;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (char16_t)(c + 32);
  if (c == 0x130) return u'i';                                          // (the JS makes it i + a combining dot)
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c : (char16_t)(c + 1);
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? (char16_t)(c + 1) : c;
  if (c == 0x178) return 0xFF;
  if (c == 0x386) return 0x3AC;
  if (c >= 0x388 && c <= 0x38A) return (char16_t)(c + 37);
  if (c == 0x38C) return 0x3CC;
  if (c == 0x38E || c == 0x38F) return (char16_t)(c + 63);
  if ((c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB)) return (char16_t)(c + 32);
  if (c >= 0x410 && c <= 0x42F) return (char16_t)(c + 32);
  if (c >= 0x400 && c <= 0x40F) return (char16_t)(c + 80);
  if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF)) return (c & 1) ? c : (char16_t)(c + 1);
  return c;
}
std::u16string jsLower(std::u16string s) { for (auto& c : s) c = lowerCh(c); return s; }
bool startsWith(const std::u16string& s, const std::u16string& p) { return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin()); }
// .replace(/^\s*[-:;,]\s*/, '')
std::u16string stripSep(const std::u16string& t) {
  size_t i = 0; while (i < t.size() && jsSpace(t[i])) i++;
  if (i >= t.size() || !(t[i] == u'-' || t[i] == u':' || t[i] == u';' || t[i] == u',')) return t;
  i++; while (i < t.size() && jsSpace(t[i])) i++;
  return t.substr(i);
}
// oneLine: t.trim().replace(/\s*\n\s*/g, ' / '): a run of white space that holds a newline becomes " / "
std::u16string oneLine(const std::u16string& s) {
  const std::u16string t = jsTrim(s);
  std::u16string o;
  for (size_t i = 0; i < t.size();) {
    if (!jsSpace(t[i])) { o.push_back(t[i++]); continue; }
    size_t j = i; bool nl = false;
    while (j < t.size() && jsSpace(t[j])) { if (t[j] == u'\n') nl = true; j++; }
    if (nl) o += u" / "; else o.append(t, i, j - i);
    i = j;
  }
  return o;
}
bool truthy(const OJ* v) {
  if (!v || v->is_null()) return false;
  if (v->is_boolean()) return v->get<bool>();
  if (v->is_number_float()) { const double d = v->get<double>(); return d == d && d != 0; }
  if (v->is_number()) return v->get<int64_t>() != 0;
  if (v->is_string()) return !v->get_ref<const std::string&>().empty();
  return true;                                                          // arrays and objects, empty or not
}
const OJ* field(const OJ& o, const char* k) { if (!o.is_object()) return nullptr; auto it = o.find(k); return it == o.end() ? nullptr : &*it; }
std::string sfield(const OJ& o, const char* k) { const OJ* v = field(o, k); return v && v->is_string() ? v->get<std::string>() : std::string(); }
// a value as a template literal prints it (`${x}`): undefined (no value) -> "undefined"
std::string jsString(const OJ* v) {
  if (!v) return "undefined";
  if (v->is_string()) return v->get<std::string>();
  if (v->is_null()) return "null";
  if (v->is_boolean()) return v->get<bool>() ? "true" : "false";
  if (v->is_number()) return v->dump();
  if (v->is_array()) { std::string o; bool first = true; for (auto& x : *v) { if (!first) o += ","; first = false; if (!x.is_null()) o += jsString(&x); } return o; }
  return "[object Object]";
}

// ---------------------------------------------------------------- a small HTML parser (the subset the skill writes)
struct Node {
  std::string tag;                       // '' = a text node
  std::string text;                      // a text node's text, entities decoded
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<Node*> kids;
  Node* parent = nullptr;
  const std::string* attr(const char* n) const { for (auto& a : attrs) if (a.first == n) return &a.second; return nullptr; }
  std::string at(const char* n) const { const std::string* v = attr(n); return v ? *v : std::string(); }
  bool cls(const char* c) const {
    const std::string* v = attr("class"); if (!v) return false;
    const size_t n = std::strlen(c);
    for (size_t i = 0; i < v->size();) {
      while (i < v->size() && (*v)[i] == ' ') i++;
      size_t j = i; while (j < v->size() && (*v)[j] != ' ') j++;
      if (j - i == n && v->compare(i, n, c) == 0) return true;
      i = j;
    }
    return false;
  }
};

void putCp(std::string& o, uint32_t cp) {
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
  else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}
std::string decode(const std::string& s) {
  if (s.find('&') == std::string::npos) return s;
  static const std::map<std::string, uint32_t> named = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 }, { "middot", 0xB7 },
    { "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "rdquo", 0x201D },
    { "ldquo", 0x201C }, { "times", 0xD7 }, { "larr", 0x2190 }, { "rarr", 0x2192 }, { "bull", 0x2022 }, { "copy", 0xA9 } };
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '&') { o += s[i]; continue; }
    const size_t semi = s.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 12) { o += '&'; continue; }
    const std::string ent = s.substr(i + 1, semi - i - 1);
    uint32_t cp = 0; bool ok = false;
    if (!ent.empty() && ent[0] == '#') {
      const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
      const std::string digits = ent.substr(hex ? 2 : 1);
      if (!digits.empty() && digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") == std::string::npos) { cp = (uint32_t)std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10); ok = true; }
    } else { auto it = named.find(ent); if (it != named.end()) { cp = it->second; ok = true; } }
    if (!ok) { o += '&'; continue; }
    putCp(o, cp); i = semi;
  }
  return o;
}
bool isVoid(const std::string& t) {
  static const char* v[] = { "br", "hr", "img", "input", "meta", "link", "wbr", "col", "area", "base", "source", "embed", "param", "track" };
  for (const char* x : v) if (t == x) return true;
  return false;
}
std::string lowerA(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }

class Dom {
 public:
  Node* root;
  explicit Dom(const std::string& s) {
    nodes_.emplace_back(); root = &nodes_.back(); root->tag = "#root";
    Node* cur = root;
    const size_t n = s.size();
    size_t i = 0;
    auto text = [&](const std::string& t) {
      if (t.empty()) return;
      if (!cur->kids.empty() && cur->kids.back()->tag.empty()) { cur->kids.back()->text += t; return; }
      nodes_.emplace_back(); Node* x = &nodes_.back(); x->text = t; x->parent = cur; cur->kids.push_back(x);
    };
    while (i < n) {
      if (s[i] != '<') { size_t j = s.find('<', i); if (j == std::string::npos) j = n; text(decode(s.substr(i, j - i))); i = j; continue; }
      if (s.compare(i, 4, "<!--") == 0) { size_t j = s.find("-->", i + 4); i = j == std::string::npos ? n : j + 3; continue; }
      if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) { size_t j = s.find('>', i); i = j == std::string::npos ? n : j + 1; continue; }
      if (i + 1 < n && s[i + 1] == '/') {
        size_t j = s.find('>', i); if (j == std::string::npos) { i = n; break; }
        std::string name = lowerA(s.substr(i + 2, j - i - 2)); while (!name.empty() && (name.back() == ' ' || name.back() == '\n' || name.back() == '\t' || name.back() == '\r')) name.pop_back();
        for (Node* p = cur; p && p != root; p = p->parent) if (p->tag == name) { cur = p->parent; break; }   // close it (and anything left open inside it)
        i = j + 1; continue;
      }
      if (i + 1 >= n || !std::isalpha((unsigned char)s[i + 1])) { text("<"); i++; continue; }
      // a start tag: its name, its attributes
      size_t j = i + 1; while (j < n && (std::isalnum((unsigned char)s[j]) || s[j] == '-')) j++;
      nodes_.emplace_back(); Node* el = &nodes_.back(); el->tag = lowerA(s.substr(i + 1, j - i - 1)); el->parent = cur;
      bool selfClose = false;
      while (j < n && s[j] != '>') {
        if (s[j] == '/' ) { selfClose = true; j++; continue; }
        if (s[j] == ' ' || s[j] == '\n' || s[j] == '\t' || s[j] == '\r') { j++; continue; }
        size_t a = j; while (j < n && s[j] != '=' && s[j] != '>' && s[j] != ' ' && s[j] != '\n' && s[j] != '\t' && s[j] != '\r' && !(s[j] == '/' && j + 1 < n && s[j + 1] == '>')) j++;
        std::string an = lowerA(s.substr(a, j - a)), av;
        while (j < n && (s[j] == ' ' || s[j] == '\n' || s[j] == '\t' || s[j] == '\r')) j++;
        if (j < n && s[j] == '=') {
          j++; while (j < n && (s[j] == ' ' || s[j] == '\n' || s[j] == '\t' || s[j] == '\r')) j++;
          if (j < n && (s[j] == '"' || s[j] == '\'')) { const char q = s[j]; size_t e = s.find(q, j + 1); if (e == std::string::npos) e = n; av = decode(s.substr(j + 1, e - j - 1)); j = e < n ? e + 1 : n; }
          else { size_t e = j; while (e < n && s[e] != ' ' && s[e] != '>' && s[e] != '\n' && s[e] != '\t') e++; av = decode(s.substr(j, e - j)); j = e; }
        }
        if (!an.empty()) { selfClose = false; el->attrs.emplace_back(an, av); }
      }
      i = j < n ? j + 1 : n;
      cur->kids.push_back(el);
      if (el->tag == "script" || el->tag == "style" || el->tag == "textarea" || el->tag == "title") {   // raw text up to its end tag
        const std::string end = "</" + el->tag;
        size_t e = i; for (;;) { e = s.find(end, e); if (e == std::string::npos || e + end.size() >= n || s[e + end.size()] == '>' || s[e + end.size()] == ' ') break; e += end.size(); }
        if (e == std::string::npos) e = n;
        const std::string body = s.substr(i, e - i);
        if (!body.empty()) { nodes_.emplace_back(); Node* x = &nodes_.back(); x->text = el->tag == "script" || el->tag == "style" ? body : decode(body); x->parent = el; el->kids.push_back(x); }
        size_t gt = e < n ? s.find('>', e) : std::string::npos;
        i = gt == std::string::npos ? n : gt + 1;
        continue;
      }
      if (!isVoid(el->tag) && !selfClose) cur = el;
    }
  }
  static void all(Node* n, const std::string& tag, std::vector<Node*>& out) { for (Node* k : n->kids) { if (k->tag == tag) out.push_back(k); all(k, tag, out); } }
  static Node* first(Node* n, const std::string& tag, const char* cls = nullptr) {
    for (Node* k : n->kids) { if (k->tag == tag && (!cls || k->cls(cls))) return k; if (Node* f = first(k, tag, cls)) return f; }
    return nullptr;
  }
  static std::string textOf(const Node* n) { if (n->tag.empty()) return n->text; std::string o; for (Node* k : n->kids) o += textOf(k); return o; }
 private:
  std::deque<Node> nodes_;               // stable addresses
};

// ---- inline content -> lines of runs (css white-space: normal: runs of ASCII white space are one space)
struct Style { bool bold = false, code = false; std::string href; };
void inlineOf(const Node* n, Style st, std::vector<Line>& lines, bool skipChips) {
  for (const Node* k : n->kids) {
    if (k->tag.empty()) {
      std::string t; bool sp = false;
      for (char c : k->text) { if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') { if (!sp) t += ' '; sp = true; } else { t += c; sp = false; } }
      if (t.empty()) continue;
      if (lines.empty()) lines.emplace_back();
      Run r; r.text = t; r.bold = st.bold; r.code = st.code; r.href = st.href;
      lines.back().push_back(r);
      continue;
    }
    if (k->tag == "br") { if (lines.empty()) lines.emplace_back(); lines.emplace_back(); continue; }
    if (k->tag == "script" || k->tag == "style" || k->tag == "textarea" || k->tag == "button") continue;
    if (skipChips && k->tag == "span" && k->cls("chip")) continue;
    Style s2 = st;
    if (k->tag == "b" || k->tag == "strong") s2.bold = true;
    if (k->tag == "code") s2.code = true;
    if (k->tag == "a") s2.href = k->at("href");
    inlineOf(k, s2, lines, skipChips);
  }
}
// one line, tidied as the browser shows it: no space at either end, never two in a row across runs, runs of one
// style merged
void tidy(Line& l) {
  Line o;
  for (auto& r : l) {
    if (r.text.empty()) continue;
    if (r.text[0] == ' ' && (o.empty() || (!o.back().text.empty() && o.back().text.back() == ' '))) r.text.erase(0, 1);
    if (r.text.empty()) continue;
    if (!o.empty() && o.back().bold == r.bold && o.back().code == r.code && o.back().href == r.href) o.back().text += r.text;
    else o.push_back(r);
  }
  while (!o.empty() && !o.back().text.empty() && o.back().text.back() == ' ') { o.back().text.pop_back(); if (o.back().text.empty()) o.pop_back(); }
  l.swap(o);
}
std::vector<Line> linesOf(const Node* n, bool skipChips = false) {
  std::vector<Line> lines; inlineOf(n, Style(), lines, skipChips);
  for (auto& l : lines) tidy(l);
  return lines;
}
Line lineOf1(const Node* n, bool skipChips = false) {   // everything in one line (a <br> in it is a space)
  std::vector<Line> ls = linesOf(n, skipChips);
  Line o;
  for (size_t i = 0; i < ls.size(); i++) { if (i && !o.empty() && !ls[i].empty()) { Run sp; sp.text = " "; o.push_back(sp); } for (auto& r : ls[i]) o.push_back(r); }
  tidy(o);
  return o;
}

std::vector<Block> blocksOf(const Node* ctx) {
  std::vector<Block> out;
  for (const Node* k : ctx->kids) {
    if (k->tag.empty()) {                // loose text (render_md never writes it): a paragraph
      std::string t = k->text; if (t.find_first_not_of(" \n\t\r") == std::string::npos) continue;
      Node tmp; tmp.tag = "p"; Node txt; txt.text = t; tmp.kids.push_back(&txt);
      Block b; b.kind = Block::Para; b.lines = linesOf(&tmp); out.push_back(b);
      continue;
    }
    Block b;
    if (k->tag == "p") { b.kind = Block::Para; b.lines = linesOf(k); }
    else if (k->tag == "ul" || k->tag == "ol") { b.kind = Block::Bullets; for (const Node* li : k->kids) if (li->tag == "li") b.lines.push_back(lineOf1(li)); }
    else if (k->tag == "blockquote") { b.kind = Block::Quote; b.lines = linesOf(k); }
    else if (k->tag == "pre") { b.kind = Block::Code; b.code = Dom::textOf(k); }
    else if (k->tag == "table" || (k->tag == "div" && Dom::first(const_cast<Node*>(k), "table"))) {
      Node* t = k->tag == "table" ? const_cast<Node*>(k) : Dom::first(const_cast<Node*>(k), "table");
      b.kind = Block::Table;
      std::vector<Node*> trs; Dom::all(t, "tr", trs);
      for (Node* tr : trs) {
        std::vector<Line> cells; std::vector<bool> nums; bool head = true;
        for (Node* c : tr->kids) { if (c->tag == "th" || c->tag == "td") { cells.push_back(lineOf1(c)); nums.push_back(c->cls("num")); if (c->tag == "td") head = false; } }
        const bool inHead = tr->parent && tr->parent->tag == "thead";
        if ((inHead || (head && b.head.empty() && b.rows.empty())) && b.head.empty()) b.head = cells;
        else { b.rows.push_back(cells); b.num.push_back(nums); }
      }
    } else if (k->tag == "div") { std::vector<Block> in = blocksOf(k); out.insert(out.end(), in.begin(), in.end()); continue; }
    else { b.kind = Block::Para; b.lines = linesOf(k); }
    if (b.kind == Block::Para || b.kind == Block::Quote) { while (!b.lines.empty() && b.lines.back().empty()) b.lines.pop_back(); if (b.lines.empty()) continue; }
    out.push_back(b);
  }
  return out;
}

Card cardOf(const Node* a) {
  Card c;
  c.qid = a->at("data-qid"); if (c.qid.empty()) c.qid = a->at("id");
  c.grouped = a->cls("grouped"); c.groupFirst = a->cls("group-first"); c.groupNext = a->cls("group-next"); c.groupLast = a->cls("group-last");
  for (const Node* k : a->kids) {
    if (k->tag.empty()) continue;
    if (k->tag == "div" && k->cls("cardhead")) {
      for (const Node* h : k->kids) {
        if (h->tag == "h3") c.title = lineText(lineOf1(h));
        else if (h->tag == "span" && h->cls("chip") && !h->cls("state")) { const std::string t = lineText(lineOf1(h)); if (!t.empty()) c.chips.push_back(t); }
      }
    } else if (k->tag == "div" && k->cls("context")) c.context = blocksOf(k);
    else if (k->tag == "div" && k->cls("options")) {
      for (const Node* b : k->kids) {
        if (b->tag != "button") continue;
        Option o; o.key = b->at("data-pick"); o.rec = b->cls("rec");
        if (Node* l = Dom::first(const_cast<Node*>(b), "span", "olabel")) o.label = lineOf1(l);
        if (Node* cs = Dom::first(const_cast<Node*>(b), "span", "cons")) {
          o.cons = lineOf1(cs);
          if (!o.cons.empty() && o.cons[0].text.rfind("- ", 0) == 0) { o.cons[0].text.erase(0, 2); tidy(o.cons); }
          else if (!o.cons.empty() && o.cons[0].text == "-") { o.cons.erase(o.cons.begin()); tidy(o.cons); }
        }
        if (o.label.empty() && !b->attr("data-pick")) continue;
        c.options.push_back(o);
      }
    } else if (k->tag == "div" && k->cls("replies")) {
      for (const Node* b : k->kids) {
        if (b->tag != "button") continue;
        Reply r; r.pick = b->at("data-pick"); r.label = lineText(lineOf1(b)); r.tip = b->at("title"); r.off = b->attr("disabled") != nullptr; r.none = b->cls("none");
        if (r.pick.empty()) continue;
        c.replies.push_back(r);
      }
    } else if (k->tag == "p" && k->cls("question")) {
      c.question = lineOf1(k, true);
      for (const Node* h : k->kids) if (h->tag == "span" && h->cls("chip")) c.pickAny = true;
    } else if (k->tag == "textarea") c.placeholder = k->at("placeholder");
  }
  return c;
}

void walkMain(Node* n, Page& p, bool& carried) {
  for (Node* k : n->kids) {
    if (k->tag.empty()) continue;
    if (k->tag == "article" && k->cls("card")) { Card c = cardOf(k); c.carried = carried; p.cards.push_back(c); continue; }
    if ((k->tag == "h3" || k->tag == "h2") && k->cls("subhead")) { carried = true; continue; }
    if (k->tag == "div" && k->cls("replaced")) { p.replaced = lineOf1(k); continue; }
    if (k->tag == "section" && (k->cls("summary") || k->cls("fyi"))) {
      std::vector<Line>& to = k->cls("summary") ? p.summary : p.fyi;
      std::vector<Node*> lis; Dom::all(k, "li", lis);
      for (Node* li : lis) to.push_back(lineOf1(li));
      continue;
    }
    walkMain(k, p, carried);
  }
}

OJ runJson(const Line& l) {
  OJ a = OJ::array();
  for (auto& r : l) { OJ o = { { "t", r.text } }; if (r.bold) o["b"] = true; if (r.code) o["c"] = true; if (!r.href.empty()) o["a"] = r.href; a.push_back(o); }
  return a;
}
OJ linesJson(const std::vector<Line>& ls) { OJ a = OJ::array(); for (auto& l : ls) a.push_back(runJson(l)); return a; }

}  // namespace

// ---------------------------------------------------------------- the page
std::string lineText(const Line& l) { std::string o; for (auto& r : l) o += r.text; return o; }

int Page::cardOf(const std::string& qid) const { for (size_t i = 0; i < cards.size(); i++) if (cards[i].qid == qid) return (int)i; return -1; }
int Page::itemOf(const std::string& qid) const { for (size_t i = 0; i < items.size(); i++) if (items[i].qid == qid) return (int)i; return -1; }
const ReplyDef* Page::reply(const std::string& code) const { for (auto& r : replies) if (r.code == code) return &r; return nullptr; }

Page parsePage(const std::string& html0) {
  Page p;
  // the HTML parser's own first step: CR LF and a lone CR are LF (the skill writes its pages with CRLF on Windows)
  std::string html; html.reserve(html0.size());
  for (size_t i = 0; i < html0.size(); i++) { if (html0[i] == '\r') { html += '\n'; if (i + 1 < html0.size() && html0[i + 1] == '\n') i++; } else html += html0[i]; }
  Dom dom(html);
  if (Node* t = Dom::first(dom.root, "title")) p.title = Dom::textOf(t);
  Node* data = nullptr;
  { std::vector<Node*> scripts; Dom::all(dom.root, "script", scripts); for (Node* s : scripts) if (s->at("id") == "askdata") data = s; }
  if (!data) { p.legacy = true; p.error = "the page has no askdata (an older ask-page skill made it)"; return p; }
  p.data = OJ::parse(Dom::textOf(data), nullptr, false);
  if (!p.data.is_object()) { p.data = OJ::object(); p.error = "the page's askdata is not JSON"; return p; }
  p.id = sfield(p.data, "id");
  if (const OJ* r = field(p.data, "replies")) if (r->is_object())
    for (auto it = r->begin(); it != r->end(); ++it) { ReplyDef d; d.code = it.key(); d.label = sfield(*it, "label"); d.fill = sfield(*it, "fill"); d.status = sfield(*it, "status"); p.replies.push_back(d); }
  if (const OJ* its = field(p.data, "items")) if (its->is_array())
    for (auto& i : *its) {
      Item it; it.raw = i; it.qid = sfield(i, "qid"); it.title = sfield(i, "title"); it.question = sfield(i, "question");
      it.multi = truthy(field(i, "multi"));
      if (const OJ* os = field(i, "options")) if (os->is_array()) for (auto& o : *os) {
        const OJ* k = field(o, "key");
        it.options.emplace_back(k && k->is_string() ? k->get<std::string>() : jsString(k), sfield(o, "label"));
      }
      p.items.push_back(it);
    }
  Node* main = Dom::first(dom.root, "main");
  bool carried = false;
  walkMain(main ? main : dom.root, p, carried);
  for (auto& c : p.cards) c.item = p.itemOf(c.qid);
  p.ok = !p.cards.empty() || !p.items.empty();
  if (!p.ok) p.error = "the page has no cards";
  return p;
}

// { ok, legacy, error, id, title, replaced: line, summary: [line], fyi: [line], cards: [{ qid, title, chips, carried,
//   group: '' | 'first' | 'next' (+ ' last'), context: [block], options: [{ key, label, cons, rec }], replies: [{ pick,
//   label, tip, off, none }], question, pickAny, placeholder, item }], items: D.items, replies: D.replies }
// a line = [{ t, b?, c?, a? }] (b bold, c code, a the link); a block = { kind: p | ul | quote | pre | table, lines |
// code | head + rows + num }
OJ pageJson(const Page& p) {
  OJ o = { { "ok", p.ok }, { "legacy", p.legacy }, { "error", p.error }, { "id", p.id }, { "title", p.title }, { "replaced", runJson(p.replaced) },
           { "summary", linesJson(p.summary) }, { "fyi", linesJson(p.fyi) }, { "cards", OJ::array() } };
  for (auto& c : p.cards) {
    OJ cj = { { "qid", c.qid }, { "title", c.title }, { "chips", c.chips }, { "carried", c.carried },
              { "group", !c.grouped ? std::string() : std::string(c.groupFirst ? "first" : c.groupNext ? "next" : "") + (c.groupLast ? " last" : "") } };
    OJ ctx = OJ::array();
    for (auto& b : c.context) {
      OJ bj;
      switch (b.kind) {
        case Block::Para: bj = { { "kind", "p" }, { "lines", linesJson(b.lines) } }; break;
        case Block::Bullets: bj = { { "kind", "ul" }, { "lines", linesJson(b.lines) } }; break;
        case Block::Quote: bj = { { "kind", "quote" }, { "lines", linesJson(b.lines) } }; break;
        case Block::Code: bj = { { "kind", "pre" }, { "code", b.code } }; break;
        case Block::Table: {
          OJ rows = OJ::array(); for (auto& r : b.rows) rows.push_back(linesJson(r));
          bj = { { "kind", "table" }, { "head", linesJson(b.head) }, { "rows", rows }, { "num", b.num } }; break;
        }
      }
      ctx.push_back(bj);
    }
    cj["context"] = ctx;
    OJ opts = OJ::array(); for (auto& op : c.options) opts.push_back({ { "key", op.key }, { "label", runJson(op.label) }, { "cons", runJson(op.cons) }, { "rec", op.rec } });
    cj["options"] = opts;
    OJ reps = OJ::array(); for (auto& r : c.replies) reps.push_back({ { "pick", r.pick }, { "label", r.label }, { "tip", r.tip }, { "off", r.off }, { "none", r.none } });
    cj["replies"] = reps;
    cj["question"] = runJson(c.question); cj["pickAny"] = c.pickAny; cj["placeholder"] = c.placeholder; cj["item"] = c.item;
    o["cards"].push_back(cj);
  }
  const OJ* its = field(p.data, "items"); o["items"] = its ? *its : OJ::array();
  const OJ* rs = field(p.data, "replies"); o["replies"] = rs ? *rs : OJ::object();
  return o;
}

// ---------------------------------------------------------------- page.html's logic
namespace {
const OJ* entry(const OJ& S, const std::string& q) { const OJ* s = field(S, q.c_str()); return s && s->is_object() ? s : nullptr; }
bool isReply(const OJ& p) { return p.is_string() && p.get_ref<const std::string&>().rfind("r:", 0) == 0; }
std::string labelOf(const Item& it, const std::string& k) { for (auto& o : it.options) if (o.first == k) return o.second; return ""; }
// fillOf: what a pick puts at the start of the box: an option's label (several: joined), a reply's phrase
std::string fillOf(const Page& p, const Item& it, const OJ& pick) {
  if (!truthy(&pick)) return "";
  if (pick.is_array()) {
    std::string o; bool first = true;
    for (auto& op : it.options) {
      bool in = false; for (auto& x : pick) if (x.is_string() && x.get_ref<const std::string&>() == op.first) in = true;
      if (!in) continue;
      if (!first) o += ", ";
      first = false; o += op.second;
    }
    return o;
  }
  if (!pick.is_string()) return "";
  const std::string& k = pick.get_ref<const std::string&>();
  if (k.rfind("r:", 0) == 0) { const ReplyDef* r = p.reply(k.substr(2)); return r ? r->fill : ""; }
  return labelOf(it, k);
}
std::vector<std::u16string> allFills(const Page& p, const Item& it) {
  std::vector<std::u16string> f;
  for (auto& o : it.options) if (!o.second.empty()) f.push_back(u16(o.second));
  for (auto& r : p.replies) if (!r.fill.empty()) f.push_back(u16(r.fill));
  std::stable_sort(f.begin(), f.end(), [](const std::u16string& a, const std::u16string& b) { return a.size() > b.size(); });
  return f;
}
std::u16string stripFill(const Page& p, const Item& it, const std::u16string& text) {
  const std::u16string t = trimStart(text), lt = jsLower(t);
  for (auto& f : allFills(p, it)) if (startsWith(lt, jsLower(f))) return stripSep(t.substr(std::min(f.size(), t.size())));
  return t;
}
}  // namespace

std::string State::text(const std::string& qid) const {
  const OJ* s = entry(S_, qid); const OJ* t = s ? field(*s, "text") : nullptr;
  return t && t->is_string() ? t->get<std::string>() : std::string();
}
OJ State::pickOf(const std::string& qid) const {
  const OJ* s = entry(S_, qid); const OJ* p = s ? field(*s, "pick") : nullptr;
  if (!p) return OJ();
  if (p->is_array()) return p->empty() ? OJ() : *p;
  return truthy(p) ? *p : OJ();
}
std::string State::status(const std::string& qid) const {
  const OJ p = pickOf(qid);
  const std::u16string t = jsTrim(u16(text(qid)));
  const OJ* s = entry(S_, qid);
  if (s && truthy(field(*s, "later")) && !truthy(&p)) return "later";   // older pages' Later flag
  if (isReply(p)) {
    const std::string code = p.get<std::string>().substr(2);
    if (code == "none" && t.empty()) return "skipped";
    const ReplyDef* r = p_ ? p_->reply(code) : nullptr;
    if (r) return r->status;
    return t.empty() ? "skipped" : "answered";                         // (a code the page does not know: the JS throws here)
  }
  if (truthy(&p)) return "answered";
  return t.empty() ? "skipped" : "answered";
}
std::string State::stateLabel(const std::string& qid) const { const std::string s = status(qid); return s == "skipped" ? "open" : s; }
bool State::checked(const std::string& qid, const std::string& k) const {
  const OJ p = pickOf(qid);
  if (p.is_array()) { for (auto& x : p) if (x.is_string() && x.get_ref<const std::string&>() == k) return true; return false; }
  return p.is_string() && p.get_ref<const std::string&>() == k;
}
std::string State::line(const std::string& qid) const {
  const int i = p_ ? p_->itemOf(qid) : -1;
  const OJ* raw = i >= 0 ? &p_->items[i].raw : nullptr;
  const std::string t = u8(oneLine(u16(text(qid))));
  return jsString(raw ? field(*raw, "qid") : nullptr) + " (" + jsString(raw ? field(*raw, "title") : nullptr) + "): " + (!t.empty() ? t : status(qid) == "later" ? std::string("later") : std::string());
}

void State::load(const OJ* state, bool readonly) {
  if (state && truthy(state)) S_ = state->is_object() ? *state : OJ::object();   // applyState: a copy of the host's
  if (readonly) locked_ = true;
}
bool State::clear() { if (locked_) return false; S_ = OJ::object(); return true; }

bool State::pick(const std::string& qid, const std::string& k, bool* focusBox) {
  if (focusBox) *focusBox = false;
  if (locked_ || !p_) return false;
  const int ci = p_->cardOf(qid), ii = p_->itemOf(qid);
  if (ci < 0 || ii < 0) return false;
  const Card& c = p_->cards[ci]; const Item& it = p_->items[ii];
  bool has = false;
  for (auto& o : c.options) if (o.key == k) has = true;
  for (auto& r : c.replies) if (r.pick == k) { if (r.off) return false; has = true; }
  if (!has) return false;
  const OJ was = pickOf(qid);
  OJ now;
  if (it.multi && k.rfind("r:", 0) != 0) {                // several options may be picked
    std::vector<std::string> cur;
    if (was.is_array()) for (auto& x : was) cur.push_back(x.is_string() ? x.get<std::string>() : jsString(&x));
    const bool in = std::find(cur.begin(), cur.end(), k) != cur.end();
    if (in) cur.erase(std::remove(cur.begin(), cur.end(), k), cur.end()); else cur.push_back(k);
    now = OJ::array();
    for (auto& o : it.options) if (std::find(cur.begin(), cur.end(), o.first) != cur.end()) now.push_back(o.first);
    if (now.empty()) now = OJ();
  } else now = (was.is_string() && was.get_ref<const std::string&>() == k) ? OJ() : OJ(k);   // a second click unpicks
  const std::u16string value = u16(text(qid));
  const std::u16string prev = u16(fillOf(*p_, it, was)), cur = trimStart(value);
  const std::u16string note = !prev.empty() && startsWith(jsLower(cur), jsLower(prev)) ? stripSep(cur.substr(std::min(prev.size(), cur.size()))) : stripFill(*p_, it, value);
  const std::u16string fill = u16(fillOf(*p_, it, now));   // their own words are kept
  const std::u16string box = !fill.empty() && !note.empty() ? fill + u" - " + note : (!fill.empty() ? fill : note);
  OJ e = OJ::object(); e["text"] = u8(box); e["pick"] = now;
  S_[qid] = e;
  if (focusBox) *focusBox = now.is_string() && now.get_ref<const std::string&>() == "r:none";
  return true;
}
bool State::type(const std::string& qid, const std::string& text) {
  if (locked_) return false;
  OJ e = OJ::object(); e["text"] = text; e["pick"] = pickOf(qid);
  S_[qid] = e;
  return true;
}

State::Progress State::progress() const {
  Progress g;
  if (!p_) return g;
  for (auto& i : p_->items) {
    const std::string s = status(i.qid);
    if (s == "answered" || s == "dropped") g.answered++; else if (s == "later") g.later++;
    if (const OJ* e = entry(S_, i.qid)) {
      const OJ* t = field(*e, "text");
      if (truthy(field(*e, "pick")) || (t && t->is_string() && !jsTrim(u16(t->get<std::string>())).empty()) || truthy(field(*e, "later"))) g.drafted++;
    }
  }
  g.total = (int)p_->items.size();
  return g;
}
std::string State::countText() const {
  const Progress g = progress();
  return std::to_string(g.answered) + " of " + std::to_string(g.total) + " answered" + (g.later ? ", " + std::to_string(g.later) + " later" : "");
}
OJ State::progressJson(bool withState) const {
  const Progress g = progress();
  OJ o = OJ::object();
  if (withState) o["state"] = S_;
  o["answered"] = g.answered; o["later"] = g.later; o["total"] = g.total; o["drafted"] = g.drafted;
  return o;
}

OJ State::answers(const std::string& savedAt) const {
  OJ list = OJ::array(), qids = OJ::array(), lines = OJ::array();
  int answered = 0, later = 0;
  const OJ D = p_ ? p_->data : OJ::object();
  if (p_) for (auto& i : p_->items) {
    const std::string s = status(i.qid);
    const std::u16string t16 = jsTrim(u16(text(i.qid)));
    OJ x = OJ::object();
    for (const char* k : { "qid", "key", "title", "question" }) if (const OJ* v = field(i.raw, k)) x[k] = *v;   // (undefined: no key, as JSON.stringify leaves it out)
    x["status"] = s;
    OJ choice;
    if (s == "answered") {                                          // choiceOf
      const OJ p = pickOf(i.qid);
      if (truthy(&p) && !isReply(p)) choice = p;
      else if (!truthy(&p)) {
        const std::u16string a = jsLower(jsTrim(t16));
        for (auto& o : i.options) { const std::u16string l = jsLower(jsTrim(u16(o.second))); if (a == l || (!l.empty() && startsWith(a, l))) { choice = o.first; break; } }
      }
    }
    x["choice"] = choice;
    const OJ p = pickOf(i.qid);
    x["reply"] = isReply(p) ? OJ(p.get<std::string>().substr(2)) : OJ();
    x["answer"] = !t16.empty() ? u8(t16) : s == "later" ? std::string("later") : std::string();
    if (s == "answered" || s == "dropped") answered++; else if (s == "later") later++;
    if (s != "skipped") lines.push_back(line(i.qid));
    const OJ* q = field(i.raw, "qid"); qids.push_back(q ? *q : OJ());
    list.push_back(x);
  }
  OJ A = OJ::object();
  A["kind"] = "ask-page-answers"; A["version"] = 2;
  for (const char* k : { "id", "session_id", "session_name", "account", "round" }) if (const OJ* v = field(D, k)) A[k] = *v;
  if (const OJ* v = field(D, "asked_at")) A["asked_at"] = *v;
  A["saved_at"] = savedAt;
  if (const OJ* v = field(D, "items_hash")) A["items_hash"] = *v;
  A["question_ids"] = qids;
  A["answered"] = answered; A["later"] = later; A["total"] = (int)list.size();
  A["answers"] = list; A["lines"] = lines;
  return A;
}
std::string State::asText(const OJ& A) {
  std::vector<std::string> open;
  if (const OJ* as = field(A, "answers")) if (as->is_array())
    for (auto& x : *as) if (sfield(x, "status") == "skipped") open.push_back(jsString(field(x, "qid")) + " (" + jsString(field(x, "title")) + ")");
  std::vector<std::string> out = { jsString(field(A, "id")),
    jsString(field(A, "session_name")) + " (" + jsString(field(A, "account")) + ") round " + jsString(field(A, "round")) + ", saved " + jsString(field(A, "saved_at")) };
  const OJ* later = field(A, "later");
  out.push_back(jsString(field(A, "answered")) + " of " + jsString(field(A, "total")) + " answered" + (truthy(later) ? ", " + jsString(later) + " later" : ""));
  out.push_back("");
  if (const OJ* ls = field(A, "lines")) if (ls->is_array()) for (auto& l : *ls) out.push_back(jsString(&l));
  if (!open.empty()) { std::string j; for (size_t i = 0; i < open.size(); i++) { if (i) j += ", "; j += open[i]; } out.push_back(""); out.push_back("not answered: " + j); }
  out.push_back("");
  std::string o; for (size_t i = 0; i < out.size(); i++) { if (i) o += "\n"; o += out[i]; }
  return o;
}

std::string isoTime(int64_t ms) {
  int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
  int64_t rem = ms - days * 86400000;
  // civil_from_days (Howard Hinnant)
  int64_t z = days + 719468; const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400; const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153; const unsigned d = doy - (153 * mp + 2) / 5 + 1; const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) y++;
  char b[40];
  std::snprintf(b, sizeof b, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", (int)y, m, d, (int)(rem / 3600000), (int)(rem / 60000 % 60), (int)(rem / 1000 % 60), (int)(rem % 1000));
  return b;
}
std::string stringify(const OJ& v, int indent) { return v.dump(indent, ' ', false, nlohmann::detail::error_handler_t::replace); }

}}}  // namespace deck::core::askcards
