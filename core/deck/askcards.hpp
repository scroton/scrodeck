// core/deck/askcards.hpp — an ask page as NATIVE cards: the page's data -> a card model, and the page's own logic
// (the ask-page skill's templates/page.html, its script) over that model, so a shell can draw the cards itself
// instead of running a browser for them (2026-11-23: "native ask cards ... in whichever is most performant").
//
// WHERE THE MODEL COMES FROM: the page file itself, as the deck serves it (the in-process deck: the file; a remote
// of another deck: GET /ask/<page id>.html, lib/phone.js). ask_page.py renders the cards' text server-side (its
// markdown-lite render_md / inline) and embeds the logic's data as JSON (<script id="askdata">, the page's `D`), so:
//   * D (parsed as it is): the ids, the replies (label, fill, status), the items (qid, key, title, question,
//     options {key, label}, recommend, multi) - everything answers() reads;
//   * the HTML of <main> (a small tolerant parser for the subset render_page / render_card write): the summary, the
//     FYI, the "Replaced by" banner, and per card its head (qid, title, chips), its context as blocks (paragraphs
//     with <br> lines, bullets, quotes, code blocks, tables with their number cells) of inline runs (bold, code,
//     links), its option rows (key, label, consequence, the recommended one), its reply buttons IN THE PAGE'S ORDER
//     (the skill's reply_order) with their tips and the ones that are off, its question, "pick any".
// A page with no askdata (an older skill's page, from before the host bridge) parses as `legacy`: the shell says so,
// as the web pane does for a page that never says "ready".
//
// THE STATE is page.html's S, exactly: { <qid>: { text, pick } } with pick null | "<option key>" | ["a", "c"]
// (multi) | "r:<reply code>", plus an older page's `later` flag. So the drafts round-trip with every other view of
// the same page (the Electron deck's iframes, the phone's ask screen): the deck keeps them (askDraft / askDraftGet),
// the shell hands them in with load() and sends state() back after each change, as the page's host bridge does.
// Every rule is page.html's, line for line, on UTF-16 strings as the JS has them (lengths, slices, trim, \s):
// statusOf, choiceOf, the pick toggle with its fill / strip of the box's text, lineOf, answers(), asText().
// The one approximation: toLowerCase is the JS's for ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic (all a
// label is compared with in practice); other scripts compare as they are.
//
// PURE: no files, no clock (saved_at is the caller's), no platform. Checked against page.html's own script run
// headless (core/test/deck/gen-askcards.js -> core/test/fixtures/deck/askcards.json) and against the skill's
// `ask_page.py read` (core/test/deck/askcards-read.js).
#pragma once
#include "../third_party/nlohmann/json.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace deck { namespace core { namespace askcards {

using OJ = nlohmann::ordered_json;      // the JS's key order (insertion), as JSON.stringify writes it

// ---- the model
struct Run { std::string text; bool bold = false, code = false; std::string href; };   // href '' = not a link
using Line = std::vector<Run>;

struct Block {
  enum Kind { Para, Bullets, Quote, Code, Table };
  Kind kind = Para;
  std::vector<Line> lines;               // Para / Quote: its lines (each <br> starts one); Bullets: one per item
  std::string code;                      // Code: the text, its newlines kept (css pre-wrap)
  std::vector<Line> head;                // Table: the header cells
  std::vector<std::vector<Line>> rows;   // Table: the body rows' cells
  std::vector<std::vector<bool>> num;    // Table: the body cell is a number (td.num: right-aligned, no wrap)
};

struct Option { std::string key; Line label, cons; bool rec = false; };   // cons: the consequence ('' = none)
struct Reply { std::string pick, label, tip; bool off = false, none = false; };   // pick "r:<code>"; off = disabled

struct Card {
  std::string qid, title;
  std::vector<std::string> chips;        // the head's chips in order: "from round 2", "explains Q3a-Q3c", "see Q3a above"
  bool carried = false;                  // under "Still open from earlier rounds"
  bool grouped = false, groupFirst = false, groupNext = false, groupLast = false;   // a related series (Q3a, Q3b ...)
  std::vector<Block> context;
  std::vector<Option> options;
  std::vector<Reply> replies;            // the reply buttons, in the page's order
  Line question; bool pickAny = false;   // pickAny: the "pick any" chip (a multi card)
  std::string placeholder;               // the box's placeholder ("Your answer to Q1")
  int item = -1;                         // its index in D.items (-1: the page's data does not name it)
};

struct Item {                            // D.items[i]: what the logic reads
  std::string qid, title, question;
  OJ raw;                                // the item as D has it (answers() copies key / title / question from it)
  std::vector<std::pair<std::string, std::string>> options;   // String(o.key), o.label
  bool multi = false;
};
struct ReplyDef { std::string code, label, fill, status; bool known = true; };

struct Page {
  bool ok = false;                       // parsed: there are cards to show
  bool legacy = false;                   // no askdata: an older skill's page (its own buttons answer it)
  std::string error;                     // why not ok
  OJ data = OJ::object();                // D, as the page embeds it
  std::string id, title;                 // D.id; the document's <title>
  Line replaced;                         // the "Replaced by ..." banner; empty = none
  std::vector<Line> summary, fyi;
  std::vector<Card> cards;               // the page's order: this round's cards, then the carried ones
  std::vector<Item> items;               // D.items, in D's order (answers() and the lines follow it)
  std::vector<ReplyDef> replies;         // D.replies, in its order (allFills reads them in it)
  int cardOf(const std::string& qid) const;     // index in cards, -1
  int itemOf(const std::string& qid) const;     // index in items, -1
  const ReplyDef* reply(const std::string& code) const;
};

Page parsePage(const std::string& html);
OJ pageJson(const Page& p);              // the model as JSON (tests; a shell's bridge): see askcards.cpp for the shape
std::string lineText(const Line& l);     // the plain text of a line

// ---- the state and page.html's logic over it
class State {
 public:
  explicit State(const Page* page) : p_(page) {}
  void setPage(const Page* page) { p_ = page; }
  // the host bridge, host -> page (page.html): load { state | null, readonly } / lock / clear
  void load(const OJ* state, bool readonly);     // state null = keep what is there (`if (m.state) applyState(m.state)`)
  void lock() { locked_ = true; }
  bool locked() const { return locked_; }
  bool clear();                                  // false when locked (nothing changes)
  // what the person does. pick: a click on an option or reply button (`k` = its data-pick); false = nothing
  // happened (locked, no such card, the button is off). `focusBox` is set when the page would focus the box
  // (See below: "my note below is the answer").
  bool pick(const std::string& qid, const std::string& k, bool* focusBox = nullptr);
  bool type(const std::string& qid, const std::string& text);   // the box's new value (an input event)
  // reads
  std::string text(const std::string& qid) const;              // the box's value
  OJ pickOf(const std::string& qid) const;                      // null | string | array (page.html pickOf)
  std::string status(const std::string& qid) const;            // answered | later | dropped | skipped
  std::string stateLabel(const std::string& qid) const;        // the head's chip: 'open' for skipped
  bool checked(const std::string& qid, const std::string& k) const;   // aria-checked of the button data-pick=k
  bool answered(const std::string& qid) const { const std::string s = status(qid); return s == "answered" || s == "dropped"; }
  bool later(const std::string& qid) const { return status(qid) == "later"; }
  std::string line(const std::string& qid) const;              // lineOf: the card's Copy text
  struct Progress { int answered = 0, later = 0, total = 0, drafted = 0; };
  Progress progress() const;
  std::string countText() const;                               // "2 of 5 answered, 1 later"
  OJ answers(const std::string& savedAt) const;                // page.html answers(); savedAt = new Date().toISOString()
  static std::string asText(const OJ& answers);                // page.html asText(A)
  const OJ& state() const { return S_; }
  // the page's "ready" / "change" messages to its host: { state?, answered, later, total, drafted }
  OJ progressJson(bool withState) const;
 private:
  const Page* p_;
  OJ S_ = OJ::object();
  bool locked_ = false;
};

std::string isoTime(int64_t msSinceEpoch);     // new Date(ms).toISOString(): 2026-11-23T17:05:01.123Z
std::string stringify(const OJ& v, int indent = -1);   // JSON.stringify(v, null, indent)

}}}  // namespace deck::core::askcards
