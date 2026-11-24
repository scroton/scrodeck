// windows/src/asks.hpp — the ask pane (renderer/js/49-asks.js), for the native shell.
//
// Claude sessions ask their questions on ask pages (the ask-page skill: one self-contained, sandboxed HTML page
// per round). The deck sends what waits as meta.asks and what was answered as meta.askLog; this class is
// 49-asks.js's model and view logic, function for function (the names are kept: openAskPane -> openPane,
// paintAskPane -> paint, askDone -> done …), and it owns the web view the pages show in.
//
// WHO DRAWS WHAT (decided 2026-11-18, see windows/WEB-NOTES.md "Why the list is in the web view"):
//   * Gfx (Direct2D) draws the pane's bar (css #ask-pane .ap-bar: title, hint, ×) and, when there is no WebView2
//     runtime, the note that says so.
//   * ONE WebPane under the bar is the pane's scroller (css .ap-scroll). It shows a HOST document made here, from
//     a string: the session headers (sticky, as in the css), the "recently answered" rows, and one sandboxed
//     <iframe> per ask page — the same elements and the same css rules 49-asks.js and style.css make, so the
//     layout is the Electron pane's by construction. The host document decides NOTHING: every text, every
//     disabled button, every frame it holds is told to it by this class (op 'render'); it reports clicks
//     (ev 'ui') and relays the pages' postMessage protocol in both directions (ev 'page' / op 'post'), with the
//     checks 49-asks.js makes on its side (the message's source is a frame made here and names that frame's page
//     id) plus the origin (a sandboxed page's origin is opaque).
//     Why not one child window per page under headers drawn with Gfx: a page is as tall as its content and the
//     LIST scrolls, headers sticking; a wheel turn over a cross-origin frame never reaches the window that would
//     have to scroll the others, and child windows cannot be clipped under a drawn header.
//   * The pages themselves are the skill's, loaded from the deck (/ask/<id>.html, lib/phone.js) with the deck's
//     own sandbox CSP. The host document is served from memory AT THE DECK'S ORIGIN, so the deck's SameSite=Lax
//     cookie rides on the frames' requests; the token is set as that cookie (HttpOnly) through the cookie manager
//     of an InPrivate profile — never in a URL, never on disk, never in a log.
//
// The app owns one AskPane, tells it the meta, the pane's rectangle and the keys, and gives it the deck calls
// (Deps). Everything runs on the window thread. Wiring: windows/WEB-NOTES.md.
//
// NATIVE (config.json askPane "native", the default since 2026-11-23): no web view at all. askcards.hpp's CardsView
// stands in for the host document AND for every framed page: it takes the same render message, reports the same
// clicks, and speaks the pages' host bridge both ways for pages it draws itself (core/deck/askcards: the page
// file parsed, page.html's state and logic). Everything below that is not the web view's own (the sections, the
// marks, Done / Copy all / Discard, the drafts, the read-only log pages) is the same code for both. The pages come
// from the same places: the in-process deck's page file, or GET /ask/<id>.html from the deck (the same cookie).
// "web" keeps the WebView2 pane (below) as it was.
#pragma once
#include "gfx.hpp"
#include "ws.hpp"
#include "keys.hpp"
#include "askcards.hpp"
#include "../web/webpane.hpp"
#include <shlwapi.h>
#include <winhttp.h>
#include "../../core/third_party/nlohmann/json.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace deck { namespace asks {

using json = nlohmann::json;

// ---- the host document (see the head of this file). css: renderer/style.css :root, html/body, .t-ask, #ask-pane
// .ap-scroll … .ap-round.blocked, copied rule for rule; markup: 49-asks.js askSection / makeFrame / paintRecent.
inline const char* hostDocument() {
  return R"HOST(<!doctype html>
<html><head><meta charset="utf-8"><title>asks</title>
<style>
:root { --bg: #0c0c0c; --tile-border: #2c2c2c; --fg: #ececec; --fg-dim: #9a9a9a; --fg-faint: #6a6a6a; --green: #3ecf5a;
  --btn: #262626; --btn-border: #383838; --danger: #c94a4a; --ask: #6aa9ff; --ask-ink: #0b1220; --ask-dim: #4a6c99; }
* { box-sizing: border-box; }
[hidden] { display: none !important; }
html, body { margin: 0; height: 100%; background: var(--bg); color: var(--fg);
  font-family: "Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif; font-size: 13px; overflow: hidden; }
button { font: inherit; color: inherit; cursor: pointer; }
.t-ask { flex: 0 0 auto; display: inline-block; min-width: 15px; height: 13px; padding: 0 3px; border: 1px solid var(--ask);
  color: var(--ask); font-size: 10px; line-height: 11px; font-weight: 700; text-align: center; font-variant-numeric: tabular-nums;
  font-family: "Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif; }
.t-ask::before { content: "?"; margin-right: 1px; opacity: .75; }
.t-ask.drafted { background: var(--ask); color: var(--ask-ink); }
.t-ask.sent { border-color: var(--ask-dim); color: var(--ask-dim); }
.t-ask.sent::before { content: ""; margin: 0; }
#ask-pane { position: absolute; inset: 0; display: flex; flex-direction: column; background: var(--bg); outline: none; }
#ask-pane .ap-scroll { flex: 1 1 auto; overflow-y: auto; overflow-x: hidden; }
#ask-pane .ap-list { display: flex; flex-direction: column; }
#ask-pane .ap-empty { padding: 16px; color: var(--fg-dim); }
.ap-sess { border-bottom: 1px solid var(--tile-border); }
.ap-head { position: sticky; top: 0; z-index: 2; background: #141414; border-bottom: 1px solid var(--tile-border); border-left: 2px solid var(--ask); padding: 4px 8px 5px; }
.ap-who { display: flex; align-items: baseline; gap: 6px; min-width: 0; }
.ap-who .t-ask { align-self: center; }
.ap-abbr { color: var(--fg-dim); font-weight: 600; }
.ap-name { font-weight: 600; font-size: 13.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-width: 0; }
.ap-meta { color: var(--fg-faint); font-size: 11.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-width: 0; flex: 1 1 auto; }
.ap-row, .ap-confirm { display: flex; align-items: center; gap: 4px; margin-top: 4px; flex-wrap: wrap; }
.ap-head button { height: 22px; padding: 0 10px; font-size: 12px; background: var(--btn); border: 1px solid var(--btn-border); border-radius: 0; color: var(--fg); }
.ap-head button:hover:not(:disabled) { filter: brightness(1.25); }
.ap-head button:disabled { opacity: .4; cursor: default; }
.ap-head button.primary { background: var(--ask); border-color: var(--ask); color: var(--ask-ink); font-weight: 600; }
.ap-head button.danger { background: var(--danger); border-color: var(--danger); color: #fff; }
.ap-count { color: var(--fg-dim); font-size: 12px; margin-left: 6px; font-variant-numeric: tabular-nums; }
.ap-flash { color: var(--green); font-size: 12px; flex: 1 1 auto; min-width: 0; }
.ap-head button.ap-discard { margin-left: auto; background: transparent; border-color: transparent; color: var(--fg-faint); }
.ap-head button.ap-discard:hover:not(:disabled) { color: var(--danger); border-color: #3a2424; filter: none; }
.ap-confirm-text { color: var(--fg); font-size: 12px; margin-right: 8px; }
.ap-body { display: flex; flex-direction: column; padding: 0 8px 6px; }
.ap-round { display: flex; flex-direction: column; }
.ap-rnote { color: var(--fg-dim); font-size: 11.5px; padding: 6px 0 0; }
.ap-frame { display: block; width: 100%; height: 320px; border: 0; background: var(--bg); }
.ap-recent .ap-rhead { border-left-color: var(--fg-faint); }
.ap-lrows { display: flex; flex-direction: column; gap: 2px; padding-top: 4px; }
.ap-lrow { display: flex; flex-direction: column; border: 1px solid var(--tile-border); background: #121212; }
.ap-lhead { display: flex; align-items: baseline; gap: 6px; padding: 3px 6px; cursor: pointer; min-width: 0; }
.ap-lhead:hover { background: #1b1b1b; }
.ap-lchev::before { content: "\25B8"; color: var(--fg-faint); font-size: 10px; }
.ap-lrow.open .ap-lchev::before { content: "\25BE"; }
.ap-lbody { padding: 0 6px 6px; }
.ap-lines { margin: 4px 0 0; padding: 4px 6px; background: var(--bg); border: 1px solid var(--tile-border); white-space: pre-wrap; font: 12px/1.4 Consolas, "Cascadia Mono", monospace; color: var(--fg); }
.ap-round.blocked { position: relative; }
.ap-round.blocked::after { content: ""; position: absolute; inset: 0; cursor: not-allowed; }
</style></head>
<body><div id="ask-pane" tabindex="-1"><div class="ap-scroll"><div class="ap-list"></div><div class="ap-empty" hidden></div></div></div>
<script>
(function () {
  'use strict';
  const wv = window.chrome && window.chrome.webview;
  if (!wv) return;
  const tell = m => { try { wv.postMessage(m); } catch (e) { /* closing */ } };
  const pane = document.getElementById('ask-pane'), list = pane.querySelector('.ap-list'), scroll = pane.querySelector('.ap-scroll'), empty = pane.querySelector('.ap-empty');
  const frames = new Map();     // '<kind>:<page id>' -> { id, sid, kind, frame, wrap, note }
  const secs = new Map();       // session id -> { el, head, body }
  const SANDBOX = 'allow-scripts allow-popups allow-popups-to-escape-sandbox allow-downloads';   // lib/phone.js, the /ask/ route
  const set = (el, text) => { if (el.textContent !== text) el.textContent = text; };

  const recentSec = document.createElement('section'); recentSec.className = 'ap-sess ap-recent'; recentSec.style.order = '100000'; recentSec.hidden = true;
  recentSec.innerHTML = '<div class="ap-head ap-rhead"><div class="ap-who"><span class="ap-name">Recently answered</span><span class="ap-meta"></span></div></div><div class="ap-body ap-lrows"></div>';
  list.appendChild(recentSec);

  function section(sid) {
    let sec = secs.get(sid);
    if (sec) return sec;
    const el = document.createElement('section'); el.className = 'ap-sess'; el.dataset.sid = sid;
    const head = document.createElement('div'); head.className = 'ap-head';
    head.innerHTML = '<div class="ap-who"><span class="ap-mark t-ask"></span><span class="ap-abbr"></span><span class="ap-name"></span><span class="ap-meta"></span></div>'
      + '<div class="ap-row"><button type="button" class="ap-done primary">Done</button><button type="button" class="ap-copy">Copy all</button>'
      + '<button type="button" class="ap-term"></button><span class="ap-count"></span><span class="ap-flash" aria-live="polite"></span>'
      + '<button type="button" class="ap-discard">Discard answers…</button></div>'
      + '<div class="ap-confirm" hidden><span class="ap-confirm-text"></span><button type="button" class="ap-keep primary">Keep them</button><button type="button" class="ap-discard-yes danger">Discard</button></div>';
    const body = document.createElement('div'); body.className = 'ap-body';
    el.append(head, body);
    for (const [cls, what] of [['ap-done', 'done'], ['ap-copy', 'copy'], ['ap-term', 'term'], ['ap-discard', 'discard'], ['ap-keep', 'keep'], ['ap-discard-yes', 'discard-yes']])
      head.querySelector('.' + cls).addEventListener('click', ev => { if (ev.currentTarget.disabled) return; tell({ ev: 'ui', what, sid }); });
    list.appendChild(el);
    sec = { el, head, body };
    secs.set(sid, sec);
    return sec;
  }
  function makeFrame(kind, container, id, sid, src, label) {
    const key = kind + ':' + id;
    let fr = frames.get(key);
    if (fr) return fr;
    const wrap = document.createElement('div'); wrap.className = 'ap-round'; wrap.dataset.id = id;
    const note = document.createElement('div'); note.className = 'ap-rnote'; note.hidden = true;
    const frame = document.createElement('iframe');
    frame.className = 'ap-frame'; frame.title = label;
    frame.name = kind === 'ask' ? id : 'log:' + id;
    frame.setAttribute('sandbox', SANDBOX);
    frame.setAttribute('allow', 'clipboard-read; clipboard-write');
    frame.src = src;
    wrap.append(note, frame);
    container.appendChild(wrap);
    fr = { id, sid, kind, frame, wrap, note, ready: false };
    frames.set(key, fr);
    // a page made before the skill learned the host bridge never says "ready": the pane is told (49-asks.js makeFrame)
    frame.addEventListener('load', () => setTimeout(() => { if (fr.ready || frames.get(key) !== fr) return; tell({ ev: 'legacy', kind, id }); }, 1500));
    return fr;
  }
  function dropFrame(key) { const fr = frames.get(key); if (!fr) return; fr.wrap.remove(); frames.delete(key); }

  function paintMark(el, m) {
    el.hidden = !m;
    if (!m) return;
    el.classList.toggle('drafted', m.kind === 'drafted');
    el.classList.toggle('sent', m.kind === 'sent');
    set(el, m.text);
    if (el.title !== m.title) el.title = m.title;
  }
  function render(m) {
    const live = new Set(m.sections.map(s => s.sid));
    for (const [sid, sec] of secs) if (!live.has(sid)) {
      for (const [key, fr] of frames) if (fr.kind === 'ask' && fr.sid === sid) dropFrame(key);
      sec.el.remove(); secs.delete(sid);
    }
    for (const s of m.sections) {
      const sec = section(s.sid);
      sec.el.style.order = String(s.order);          // reorder by CSS, never by moving the node: a moved iframe reloads
      sec.el.hidden = !!s.hidden;
      const want = new Set(s.rounds.map(r => r.id));
      for (const [key, fr] of frames) if (fr.kind === 'ask' && fr.sid === s.sid && !want.has(fr.id)) dropFrame(key);
      for (const r of s.rounds) {
        if (!r.frame) continue;
        const fr = makeFrame('ask', sec.body, r.id, s.sid, r.src, r.label);
        fr.wrap.style.order = String(r.order);
        if (r.legacy) fr.frame.style.height = '75vh';
        fr.wrap.classList.toggle('blocked', !!r.blocked);
        fr.note.hidden = !r.note; set(fr.note, r.note || '');
      }
      const h = sec.head;
      paintMark(h.querySelector('.ap-mark'), s.mark);
      set(h.querySelector('.ap-abbr'), s.abbr); set(h.querySelector('.ap-name'), s.name); set(h.querySelector('.ap-meta'), s.meta);
      set(h.querySelector('.ap-count'), s.count);
      const done = h.querySelector('.ap-done'); done.disabled = !!s.done.disabled; set(done, s.done.text); done.title = s.done.title;
      const term = h.querySelector('.ap-term'); set(term, s.term.text); term.title = s.term.title;
      h.querySelector('.ap-discard').disabled = !!s.discardDisabled;
      set(h.querySelector('.ap-flash'), s.flash || '');
      const row = h.querySelector('.ap-row'), c = h.querySelector('.ap-confirm'), was = c.hidden;
      row.hidden = !!s.confirm; c.hidden = !s.confirm;
      if (s.confirm) {
        set(c.querySelector('.ap-confirm-text'), s.confirm.text);
        c.querySelector('.ap-discard-yes').disabled = !s.confirm.armed;
        if (was) c.querySelector('.ap-keep').focus();
      }
    }
    // the recently answered rows
    const R = m.recent;
    recentSec.hidden = !R.show;
    recentSec.querySelector('.ap-rhead').hidden = !!R.headHidden;
    set(recentSec.querySelector('.ap-meta'), R.meta);
    const rowsEl = recentSec.querySelector('.ap-lrows');
    const want = new Set(R.rows.map(e => e.id));
    for (const el of [...rowsEl.children]) if (!want.has(el.dataset.id)) { dropFrame('log:' + el.dataset.id); el.remove(); }
    for (const e of R.rows) {
      let row = null;
      for (const el of rowsEl.children) if (el.dataset.id === e.id) { row = el; break; }
      if (!row) {
        row = document.createElement('div'); row.className = 'ap-lrow'; row.dataset.id = e.id;
        row.innerHTML = '<div class="ap-lhead"><span class="ap-lchev"></span><span class="ap-abbr"></span><span class="ap-name"></span><span class="ap-meta"></span></div><div class="ap-lbody"></div>';
        row.querySelector('.ap-lhead').addEventListener('click', () => tell({ ev: 'ui', what: 'lhead', id: e.id }));
        rowsEl.appendChild(row);
      }
      row.style.order = String(e.order);
      set(row.querySelector('.ap-abbr'), e.abbr); set(row.querySelector('.ap-name'), e.name); set(row.querySelector('.ap-meta'), e.meta);
      row.classList.toggle('open', !!e.open);
      const body = row.querySelector('.ap-lbody');
      body.hidden = !e.open;
      if (!e.open || !e.frame) dropFrame('log:' + e.id);
      if (!e.open) continue;
      if (e.frame) { const fr = makeFrame('log', body, e.id, e.sid, e.src, e.label); fr.wrap.classList.toggle('blocked', !!e.blocked); }
      let pre = body.querySelector('.ap-lines');
      if (e.lines != null) {
        if (!pre) { pre = document.createElement('pre'); pre.className = 'ap-lines'; body.appendChild(pre); }
        set(pre, e.lines);
      } else if (pre) pre.remove();
    }
    empty.hidden = m.empty == null; set(empty, m.empty || '');
  }

  // ---- the bridge: a framed page <-> the native pane. Only a message from a frame made here, naming that frame's
  // page, from an opaque (sandboxed) origin or this one, goes on.
  window.addEventListener('message', ev => {
    const m = ev.data;
    if (!m || typeof m !== 'object' || typeof m.askpage !== 'string') return;
    let fr = null;
    for (const x of frames.values()) if (x.frame.contentWindow === ev.source) { fr = x; break; }
    if (!fr || m.id !== fr.id) return;
    if (ev.origin !== 'null' && ev.origin !== location.origin) return;
    if (m.askpage === 'ready') { fr.ready = true; if (m.h) fr.frame.style.height = `${m.h}px`; rescroll(); }
    else if (m.askpage === 'height') { if (m.h > 0) fr.frame.style.height = `${Math.ceil(m.h)}px`; rescroll(); return; }
    tell({ ev: 'page', kind: fr.kind, id: fr.id, msg: m });
  });
  wv.addEventListener('message', ev => {
    const m = ev.data;
    if (!m || typeof m !== 'object') return;
    if (m.op === 'render') render(m);
    else if (m.op === 'post') { const fr = frames.get(m.kind + ':' + m.id); if (fr) { try { fr.frame.contentWindow.postMessage(Object.assign({ id: fr.id }, m.msg), '*'); } catch (e) { /* reloading */ } } }
    else if (m.op === 'scroll') requestAnimationFrame(() => {
      scrollTo = m.to; scrollUntil = Date.now() + 2500;
      rescroll();
      pane.focus({ preventScroll: true });
    });
    else if (m.op === 'focus') pane.focus({ preventScroll: true });
  });
  // The row asked for, at the top. On a first open the pages above it are still loading (a frame is 320px until its
  // page says how tall it is), so the place is taken again as each one grows, for a moment, until the person
  // scrolls, clicks or types.
  let scrollTo = null, scrollUntil = 0;
  function rescroll() {
    if (Date.now() > scrollUntil) return;
    const target = scrollTo === '__recent' ? recentSec : scrollTo && secs.get(scrollTo) ? secs.get(scrollTo).el : null;
    scroll.scrollTop = target ? target.offsetTop : 0;
  }
  for (const ev of ['wheel', 'mousedown', 'keydown', 'touchstart']) window.addEventListener(ev, () => { scrollUntil = 0; }, { capture: true, passive: true });
  // where the keyboard is: Esc closes the pane unless a page has it (49-asks.js: the pane's own keydown)
  const where = () => tell({ ev: 'focus', inFrame: !!(document.activeElement && document.activeElement.tagName === 'IFRAME') });
  window.addEventListener('blur', () => setTimeout(where, 0));
  window.addEventListener('focus', where);
  tell({ ev: 'host-ready' });
})();
</script></body></html>
)HOST";
}

// SetTimer without a window: a callback on the window thread's message loop, once
struct Timers {
  static std::map<UINT_PTR, std::function<void()>>& all() { static std::map<UINT_PTR, std::function<void()>> m; return m; }
  static void CALLBACK proc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    auto it = all().find(id); if (it == all().end()) return;
    std::function<void()> f = std::move(it->second); all().erase(it);
    f();
  }
  static UINT_PTR after(UINT ms, std::function<void()> f) { UINT_PTR id = SetTimer(nullptr, 0, ms, proc); if (id) all()[id] = std::move(f); return id; }
  static void cancel(UINT_PTR id) { if (!id) return; KillTimer(nullptr, id); all().erase(id); }
};

// The native pane's page: GET <deck>/ask/<id>.html with the deck's cookie (lib/phone.js authed: `deck_phone`, the
// token encodeURIComponent'd), as the web pane's frames ask for it. On a worker thread (asks.hpp fetchPage).
inline bool askPageGet(const DeckLink& l, const std::wstring& path, std::string& body, std::string& err) {
  body.clear();
  HINTERNET s = WinHttpOpen(L"ClaudeDeckNative/asks", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) { err = "no network"; return false; }
  WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
  HINTERNET c = WinHttpConnect(s, l.host.c_str(), (INTERNET_PORT)l.port, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, l.secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
  std::string tok; { static const char* hex = "0123456789ABCDEF"; for (unsigned char ch : l.token) { if (isalnum(ch) || strchr("-_.!~*'()", ch)) tok += (char)ch; else { tok += '%'; tok += hex[ch >> 4]; tok += hex[ch & 15]; } } }
  const std::wstring hdr = L"Cookie: deck_phone=" + widen(tok) + L"\r\nAccept: text/html\r\n";
  bool ok = false;
  if (r && WinHttpSendRequest(r, hdr.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
      DWORD avail = 0; if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
      std::string chunk(avail, '\0'); DWORD got = 0;
      if (!WinHttpReadData(r, &chunk[0], avail, &got) || !got) break;
      body.append(chunk, 0, got);
      if (body.size() > (32u << 20)) { err = "too large"; break; }
    }
    ok = status == 200 && err.empty();
    if (!ok && err.empty()) err = status == 404 ? "the deck has no such page" : "HTTP " + std::to_string(status);
  } else err = "the deck did not answer";
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  if (!ok) body.clear();
  return ok;
}

// HOW LONG THE WEB VIEW LIVES. A web view is a renderer process, and the first one of the process a whole browser
// process tree (browser, GPU, network, crash handler: about 250 MB on the box this was measured on). So the pane
// has one only while it is of use:
//   * made when the pane OPENS (F7, F6 on the asks tile, a click, the menu, the palette), or a moment ahead of that
//     on a real sign of intent: the pointer resting on the asks tile for ASK_WARM_HOVER_MS (the app's timer). Never
//     because a session happens to be asking: on a deck that always has asks waiting that meant always.
//   * the pane CLOSED: the view stays ASK_WEB_GRACE_MS more (opening it again straight away is instant), then it is
//     closed, and with the last web view of the process the browser process tree goes too. 5 s since the owner
//     2026-11-23 (was 20 s): a re-open after it loads the pages again, the drafts from the deck (askDraftGet) and the
//     place from the scroll target, so nothing is lost; while hidden it is on the LOW memory target (webpane.cpp).
//   * the pane open but the window MINIMISED: hidden and suspended at once, closed after ASK_OFFSCREEN_CLOSE_MS; it
//     is made again when the window comes back (the drafts are the deck's: nothing typed is lost, askDraft).
// (The NATIVE pane, the default, has no web view: none of this applies to it.)
constexpr unsigned ASK_WEB_GRACE_MS = 5000;
constexpr unsigned ASK_OFFSCREEN_CLOSE_MS = 2 * 60000;
constexpr unsigned ASK_WARM_HOVER_MS = 150;

// What the app gives the pane: the deck's calls and the few things of its own the pane asks for.
struct Deps {
  // { t:'call', rid, fn, args } answered by ok / err (core/PROTOCOL.md): askDraftGet, askLogState, askClear, askDone
  std::function<void(const std::string& fn, const json& args, std::function<void(bool ok, const json& result, const std::string& error)> done)> call;
  std::function<void(const std::string& fn, const json& args)> fire;         // the same with rid 0: askDraft
  std::function<void(const std::string& tileId)> activate;                   // 20-terminals.js activate(id, { adopt: false })
  std::function<void()> focusTerminal;                                       // the active pane takes the keys back
  std::function<void(const std::string& text, int ms)> note;                 // 25-layout.js paneNote
  std::function<void(const std::string& text)> writeClipboard;               // deck.writeClipboard (local)
  std::function<void()> changed;                                             // repaint: the bar, the asks tile, the marks
  // a key the web view does not type with, while it has the keyboard (F6, F7, F9 … Ctrl+Shift+P): the app's own
  // key handler; true = taken
  std::function<bool(unsigned vk, bool ctrl, bool shift, bool alt)> accelerator;
  std::function<void(const std::wstring& url)> openLink;                     // not set: http(s) in the default browser
  // an older ask page's own Done (a download, main.js hookAskDownloads): the full path to save `fileName` at; '' = cancelled
  std::function<std::wstring(const std::wstring& fileName)> downloadTo;
  // the native pane's answer boxes are the app's text fields (ui::State::focusEdit): give one the keys / which has them
  std::function<void(EditBox*)> focusEdit;
  std::function<EditBox*()> focusedEdit;
};

struct Mark { bool has = false; std::string kind, text, title; };            // 49-asks.js askMarkInfo
struct MenuItem { bool has = false; std::string label; std::function<void()> run; };   // askMenuItem

class AskPane {
 public:
  // a link on an ask page was followed (target=_blank / window.open): main.js openAskLink. The app decides who opens
  // it (LOCAL: the deck's own rule, `call askOpenLink`; a remote: http(s) in this machine's default browser).
  void linkClicked(const std::wstring& url) { if (deps_.openLink) deps_.openLink(url); else web::WebPane::openExternal(url); }
  static constexpr const char* RECENT = "__recent";
  static constexpr float BAR_H = 26;          // css .ap-bar: 4 + a 17px line (13px text, baseline-aligned) + 4 + 1px rule

  AskPane() : alive_(std::make_shared<bool>(true)) {}
  ~AskPane() { *alive_ = false; for (UINT_PTR t : timers_) Timers::cancel(t); endWeb(); }

  // ---- set up
  void init(HWND parent, const Deps& deps) { parent_ = parent; deps_ = deps; initCards(); }
  // config.json askPane: "native" (the pane draws the cards itself: askcards.hpp) or "web" (the WebView2 pane).
  // Set once, before the pane first opens.
  void setNative(bool on) {
    if (on == native_) return;
    std::vector<std::string> a, l; for (auto& kv : askFrames_) a.push_back(kv.first); for (auto& kv : logFrames_) l.push_back(kv.first);
    for (auto& id : a) dropFrame(askFrames_, id);
    for (auto& id : l) dropFrame(logFrames_, id);
    if (webMade_) dropWeb();
    native_ = on; hostReady_ = on; lastRender_.clear();
    if (isOpen()) paint();
  }
  bool native() const { return native_; }
  CardsView& cards() { return view_; }
  // the native pane's answer boxes, for the app's mouse and keys (its text fields' routing)
  bool ownsEdit(const EditBox* e) const { return native_ && view_.owns(e); }
  EditBox* editAt(float x, float y, std::string* id) { return native_ && isOpen() ? view_.boxAt(x, y, id) : nullptr; }
  EditBox* editById(const std::string& id) { return native_ ? view_.boxById(id) : nullptr; }
  bool editKey(EditBox* e, UINT vk, const Mods& m) { return native_ && view_.owns(e) && view_.boxKey(e, vk, m); }
  void edited(EditBox* e) { if (native_ && view_.owns(e)) view_.reveal(e); }
  bool wheel(float x, float y, int delta) { if (!native_ || !isOpen() || !webRect().has(x, y)) return false; view_.wheel(delta); changed(); return true; }
  void blink(bool on) { blink_ = on; }
  // the deck this window is a remote of; an empty link = the in-process deck (pages are files, see WEB-NOTES)
  void setLink(const DeckLink& link) {
    const std::wstring o = link.valid() ? originOf(link) : L"https://asks.claude-deck.invalid";
    if (o == origin_ && link.token == link_.token) return;
    link_ = link; origin_ = o;
    if (native_) {                                       // another deck: its pages, from it
      std::vector<std::string> a, l; for (auto& kv : askFrames_) a.push_back(kv.first); for (auto& kv : logFrames_) l.push_back(kv.first);
      for (auto& id : a) dropFrame(askFrames_, id);
      for (auto& id : l) dropFrame(logFrames_, id);
      localPages_.clear(); lastRender_.clear();
      if (isOpen()) paint();
      return;
    }
    resetWeb();
  }
  void setKeyHelp(const std::string& zoom, const std::string& asks) { khZoom_ = zoom; khAsks_ = asks; }   // lib/platform.js keyHelp
  // tests: where the profile folder goes, and whether the pane may put its web view up at all
  web::WebPaneOptions webOptions;
  unsigned graceMs = ASK_WEB_GRACE_MS, offscreenMs = ASK_OFFSCREEN_CLOSE_MS;   // tests shorten them (--web-grace)
  web::WebPane& web() { return web_; }
  bool hostReady() const { return hostReady_; }
  // The web view made ahead of an open (the pointer rests on the asks tile: the app's timer): the browser process
  // starts, the host document loads, nothing shows. Opening the pane is then a show() and a render. Not opened
  // within the grace, it goes again.
  void prewarm() { if (native_ || webMade_ || !parent_ || isOpen()) return; ensureWeb(); web_.show(false); armIdle(); }
  bool webMade() const { return webMade_; }
  unsigned webCloses() const { return webCloses_; }       // how many times the view was closed because nobody could see it
  // start-up cost, ms: the synchronous part of create(), then (from the same start) the web view ready, the host
  // document ready. 0 = not yet.
  unsigned createCallMs() const { return createCallMs_; }
  unsigned webReadyMs() const { return webReadyMs_; }
  unsigned hostReadyMs() const { return hostReadyMs_; }
  unsigned firstPageMs() const { return firstPageMs_; }   // from the last open to its first page saying it is ready (0: none loaded since)
  // keys the web view handed over while it had the keyboard (AcceleratorKeyPressed): how many, and the last one
  unsigned acceleratorsSeen() const { return accelSeen_; }
  unsigned lastAccelerator() const { return lastAccel_; }

  // ---- the deck's state (every meta): state.asks, state.askLog, settings.asks, the tiles and accounts
  void setMeta(const json& meta) {
    asks_ = arr(meta, "asks"); askLog_ = arr(meta, "askLog");
    tiles_ = arr(meta, "tiles"); accounts_ = arr(meta, "accounts");
    const json& st = obj(meta, "settings"); cfg_ = obj(st, "asks");
    // meta.askFeed (the native core, HOST-NOTES.md "THE GUARD"): `guest` = another deck is serving the ask pages.
    // They still show, read-only: the deck refuses askDone / askDraft / askClear, so nothing here offers them.
    const bool guest = str(obj(meta, "askFeed"), "state") == "guest";
    if (guest != guest_) {
      guest_ = guest;
      guestWhy_ = guest ? str(obj(meta, "askFeed"), "reason") : std::string();
      if (guestWhy_.empty()) guestWhy_ = "another deck is serving the ask pages";
      // the pages already up are locked (guest) or loaded again unlocked (this deck serves them now): made afresh
      std::vector<std::string> ids; for (auto& kv : askFrames_) ids.push_back(kv.first);
      for (auto& id : ids) dropFrame(askFrames_, id);
      for (auto& kv : sec_) confirmOffQuiet(kv.second);
      render();
    }
    paintAsks();
  }
  bool guest() const { return guest_; }                   // another deck serves the ask pages: read-only here
  std::string guestLine() const { return "Read-only here: " + guestWhy_ + ". Answer on that deck, or in the session's terminal."; }
  bool showTile() const { return !isFalse(cfg_, "showTile"); }
  bool showMarks() const { return !isFalse(cfg_, "showMarks"); }

  // ---- the asks tile's selection (asksSelect / asksBlur)
  bool asksFocused() const { return asksFocused_; }
  const std::string& asksPick() const { return asksPick_; }
  void asksSelect(const std::string& sid) { asksFocused_ = true; asksPick_ = sid; changed(); }
  void asksBlur() { if (!asksFocused_) return; asksFocused_ = false; changed(); }
  // a click on the asks tile: row = the session id of the row clicked, RECENT, or '' for the tile itself
  void tileClick(const std::string& row) {
    if (row.empty() && mode_ == "all" && isOpen()) { closePane(); return; }
    openPane("all", row);
  }

  // ---- the pane
  bool isOpen() const { return !mode_.empty(); }
  const std::string& mode() const { return mode_; }                 // '' closed, 'all', a session id, 'log:<page id>'
  std::string logModeId() const { return mode_.rfind("log:", 0) == 0 ? mode_.substr(4) : std::string(); }

  void openPane(const std::string& mode, const std::string& scrollTo = "") {
    if (mode.empty()) return;
    if (mode != "all" && mode.rfind("log:", 0) != 0 && !askSession(mode)) { note("No questions from this session."); return; }
    mode_ = mode;
    if (mode.rfind("log:", 0) == 0) recentOpen_.insert(mode.substr(4));
    openT0_ = GetTickCount64(); firstPageMs_ = 0;
    cancel(idleTimer_);
    ensureWeb();
    if (!native_) { web_.show(!covered_ && !minimised_); placeWeb(); }
    paint();
    // every frame gets the latest drafts (another window or the phone may have moved them on)
    std::vector<std::string> ids;
    for (auto& kv : askFrames_) if (kv.second.ready && askVisible(kv.second.sid)) ids.push_back(kv.first);
    for (auto& id : ids) askLoad("ask", id);
    pendingScroll_ = mode == "all" ? scrollTo : std::string(); wantScroll_ = true;
    flushScroll();
    wantFocus_ = true;                                   // the keyboard goes to the pane (49-asks.js askPane.focus())
    if (!covered_ && !native_) web_.focus();             // (native: the app has the keys and hands them to the pane)
    armIdle();
    changed();
  }
  struct CloseOpts { std::string toTile; };
  void closePane(const CloseOpts& opts = CloseOpts()) {
    if (mode_.empty()) return;
    const std::string wasLog = logModeId();
    if (native_ && deps_.focusEdit && deps_.focusedEdit && view_.owns(deps_.focusedEdit())) deps_.focusEdit(nullptr);   // a box of the pane had the keys
    mode_.clear(); wantFocus_ = false;
    if (!native_) web_.show(false);
    snap_.clear(); snapBmp_.reset(); snapGen_++;
    if (webFocused_ && parent_) SetFocus(parent_);        // the keyboard back to the app's own window
    if (!wasLog.empty()) recentOpen_.erase(wasLog);
    for (auto& kv : sec_) confirmOffQuiet(kv.second);
    render();
    if (!opts.toTile.empty() && tileById(opts.toTile)) { if (deps_.activate) deps_.activate(opts.toTile); }
    else if (!asksFocused_ && deps_.focusTerminal) deps_.focusTerminal();
    armIdle();                                           // nobody sees the web view now: it goes after the grace
    changed();
  }

  // ---- keys (49-asks.js asksZoom / asksKey / askOpenFor / askMenuItem, deck.onAskOpen)
  // F6: true = the asks had it (25-layout.js asks them first)
  bool asksZoom() {
    if (mode_ == "all") { closePane(); return true; }
    if (asksFocused_) { openPane("all", asksPick_); return true; }
    return false;
  }
  // a session's own: its waiting ask; once that is answered, its last answers read-only, until it asks again
  bool openFor(const json* tile) {
    if (!tile || str(*tile, "kind") != "claude") return false;
    const std::string sid = str(*tile, "sessionId");
    if (askSession(sid)) { openPane(sid); return true; }
    if (const json* last = askLastLog(sid)) { openPane("log:" + str(*last, "id")); return true; }
    return false;
  }
  // F7 / Cmd+Shift+A; activeTileId = state.active
  void asksKey(const std::string& activeTileId) {
    if (isOpen()) { closePane(); return; }
    if (asksFocused_) { openPane("all", asksPick_); return; }
    if (openFor(tileById(activeTileId))) return;
    const size_t n = asks_.size();
    note(n ? "This session has no questions waiting. " + std::to_string(n) + " other session" + (n == 1 ? " has" : "s have") + ": the asks tile at the top of the sidebar." : "No questions waiting.");
  }
  // a new-ask toast was clicked (main.js notifyNewAsks -> asks:open): that session's tile, then its questions
  void onAskOpen(const std::string& sessionId, const std::string& tileId) {
    if (!tileId.empty() && tileById(tileId) && deps_.activate) deps_.activate(tileId);
    openPane(!sessionId.empty() && askSession(sessionId) ? sessionId : "all");
  }
  // a session tile's right-click (35-groups.js): its questions, or its last answers
  MenuItem menuItem(const json& t) {
    MenuItem out;
    if (str(t, "kind") != "claude") return out;
    const std::string sid = str(t, "sessionId"), tid = str(t, "id");
    if (const json* s = askSession(sid)) {
      const int n = askOpenCount(*s);
      out.has = true; out.label = std::string("Answer its question") + (n == 1 ? "" : "s") + " (" + std::to_string(n) + ")  " + khAsks_;
      out.run = [this, tid, sid] { if (deps_.activate) deps_.activate(tid); openPane(sid); };
    } else if (const json* last = askLastLog(sid)) {
      const std::string lid = str(*last, "id");
      out.has = true; out.label = "Its last answers (read-only)  " + khAsks_;
      out.run = [this, tid, lid] { if (deps_.activate) deps_.activate(tid); openPane("log:" + lid); };
    }
    return out;
  }
  // for the palette's entries (88-palette.js): does this session ask, what it is called, how many questions wait
  bool hasAsk(const std::string& sessionId) const { return askSession(sessionId) != nullptr; }
  std::string sessionName(const std::string& sessionId) const { const json* s = askSession(sessionId); return s ? askName(*s) : std::string(); }
  int openCount(const std::string& sessionId) const { const json* s = askSession(sessionId); return s ? askOpenCount(*s) : 0; }
  // Esc while the app (not the web view) has the keyboard: the pane's own keydown. true = taken. Native: the
  // pane's own keys too (Tab to the answer boxes, the scroll keys: askcards.hpp key)
  bool key(unsigned vk, const Mods& m = Mods()) {
    if (isOpen() && vk == VK_ESCAPE && !m.ctrl && !m.alt && !m.shift) { closePane(); return true; }
    if (native_ && isOpen()) return view_.key(vk, m);
    return false;
  }

  // ---- the mark on a session's tile and pane name (askMarkInfo): the page's own progress counts, as in the js
  Mark markInfo(const std::string& sessionId) const {
    Mark m;
    const json* s = sessionId.empty() ? nullptr : askSession(sessionId);
    if (!s) return m;
    const int open = askOpenCount(*s), drafted = askDrafted(*s);
    m.has = true;
    m.kind = !askUnsent(*s) ? "sent" : drafted ? "drafted" : "waiting";
    m.title = m.kind == "sent" ? "answers sent; the session has not read them yet"
      : m.kind == "drafted" ? std::to_string(drafted) + " of " + std::to_string(open) + " answered on the page, not sent yet (" + khAsks_ + ")"
      : std::to_string(open) + " question" + (open == 1 ? "" : "s") + " waiting on an ask page (" + khAsks_ + ")";
    m.text = m.kind == "sent" ? "\xE2\x9C\x93" : std::to_string(open);
    return m;
  }

  // ---- where it is and how it looks (the app calls these from its layout and its paint)
  // area: the terminal area (css #main), DIPs; scale: device px per DIP. Call when either changes.
  void layout(const RectF& area, float scale) {
    if (scale <= 0) scale = 1;
    if (area.x == area_.x && area.y == area_.y && area.w == area_.w && area.h == area_.h && scale == scale_) return;   // cheap enough to call every frame
    area_ = area; scale_ = scale; placeWeb();
  }
  RectF barRect() const { return RectF(area_.x, area_.y, area_.w, BAR_H); }
  RectF webRect() const { return RectF(area_.x, area_.y + BAR_H, area_.w, std::max(0.f, area_.h - BAR_H)); }
  // the bar's title and hint, as paintAskPane wrote them
  const std::string& barTitle() const { return barTitle_; }
  const std::string& barHint() const { return barHint_; }

  // Draws the pane over the terminal area: the background, the bar, and (no runtime) the note. The web view is a
  // child window over the rest; nothing drawn under it shows.
  void draw(Gfx& g) {
    if (!isOpen()) return;
    const Rgba bg = Rgba::hex(0x0c0c0c), fg = Rgba::hex(0xececec), dim = Rgba::hex(0x9a9a9a), faint = Rgba::hex(0x6a6a6a), line = Rgba::hex(0x2c2c2c);
    g.fill(area_, bg);
    const RectF bar = barRect();
    g.fill(RectF(bar.x, bar.y, bar.w, bar.h - 1), Rgba::hex(0x0f0f0f));
    g.fill(RectF(bar.x, bar.b() - 1, bar.w, 1), line);
    const std::wstring fam = family(g);
    Font ft; ft.family = fam; ft.size = 13; ft.weight = 600;
    Font fh; fh.family = fam; fh.size = 11.5f;
    Font fx; fx.family = fam; fx.size = 16;
    // the three sit on one baseline, 4 + 14 px under the bar's top (the 13px title's and the × button's ascent)
    const float base = bar.y + 4 + 14;
    const float xw = g.textWidth(L"\u00D7", fx) + 8;
    closeRect_ = RectF(bar.r() - 8 - xw, bar.y + 4, xw, 16);
    if (hoverClose_) g.fill(closeRect_, Rgba::hex(0x1f1f1f));
    g.text(L"\u00D7", RectF(closeRect_.x, base - 17, closeRect_.w, 21), fx, hoverClose_ ? fg : dim, Align::Center, false);
    const std::wstring title = widen(barTitle_), hint = widen(barHint_);
    const float avail = std::max(0.f, closeRect_.x - 10 - (bar.x + 8));
    const float tw = std::min(avail, g.textWidth(title, ft));
    g.text(title, RectF(bar.x + 8, base - 14, tw + 1, 17), ft, fg, Align::Left, true);
    const float hx = bar.x + 8 + tw + 10;
    if (closeRect_.x - 10 - hx > 4) g.text(hint, RectF(hx, base - 12, closeRect_.x - 10 - hx, 15), fh, faint, Align::Left, true);
    if (native_) { view_.draw(g, webRect(), blink_); return; }   // the cards, drawn here: no web view, no picture of one
    if (covered_ && !snap_.empty() && g.wic && g.rt) {
      if (!snapBmp_ || snapRt_ != g.rt.p) {
        snapBmp_.reset(); snapRt_ = g.rt.p;
        Com<IStream> st; st.p = SHCreateMemStream((const BYTE*)snap_.data(), (UINT)snap_.size());
        Com<IWICBitmapDecoder> dec; Com<IWICBitmapFrameDecode> frame; Com<IWICFormatConverter> conv;
        if (st && SUCCEEDED(g.wic->CreateDecoderFromStream(st.p, nullptr, WICDecodeMetadataCacheOnLoad, dec.put())) && dec && SUCCEEDED(dec->GetFrame(0, frame.put()))
            && SUCCEEDED(g.wic->CreateFormatConverter(conv.put())) && SUCCEEDED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
          g.rt->CreateBitmapFromWicBitmap(conv.p, nullptr, snapBmp_.put());
      }
      const RectF w = webRect();
      if (snapBmp_) g.rt->DrawBitmap(snapBmp_.p, D2D1::RectF(w.x, w.y, w.r(), w.b()), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    // nothing waiting, and the web view is not up (it is starting, or this run has none): the pane's own empty line
    // (css .ap-empty: 16px in, the dim foreground), where the host document writes it once it is there
    if (!hostReady_ && asks_.empty() && askLog_.empty() && logModeId().empty() && web_.state() != web::WebState::NoRuntime && web_.state() != web::WebState::Failed) {
      Font fe; fe.family = fam; fe.size = 13;
      g.textWrapped(L"No questions waiting. Sessions that ask on a page show up here and in the asks tile.", RectF(area_.x + 16, bar.b() + 16, std::max(40.f, area_.w - 32), area_.h), fe, dim, 0, 17.3f);
    }
    if (web_.state() == web::WebState::NoRuntime || web_.state() == web::WebState::Failed) {
      // css .ap-empty: 16px in, the dim foreground
      Font fe; fe.family = fam; fe.size = 13;
      const std::wstring why = web_.state() == web::WebState::NoRuntime
        ? std::wstring(L"The ask pane shows the sessions' ask pages in a web view, and this machine has no ") + web::runtimeDownloadName()
          + L". Install it from Microsoft (search for that name), then open the asks again. Until then, answer in the session's terminal."
        : L"The ask pane's web view did not start: " + web_.error() + L". Answer in the session's terminal, or restart the deck.";
      g.textWrapped(why, RectF(area_.x + 16, bar.b() + 16, std::max(40.f, area_.w - 32), area_.h), fe, dim, 0, 17.3f);
    }
  }
  // ---- things the app draws OVER the terminal area (the palette, a menu, a panel, a tooltip, the pane note)
  // The web view is a child window: nothing Direct2D draws shows over it. While something of the app's overlaps
  // the pane, cover(true): the web view is photographed, hidden, and draw() paints the photograph in its place (a
  // still picture, a moment late: the overlay shows over the page as it does in Electron). cover(false) brings the
  // live page back. The keyboard is the app's while covered.
  void cover(bool on) {
    if (on == covered_) return;
    if (native_) { covered_ = on; return; }              // drawn by the app: an overlay simply draws over it
    covered_ = on; snapGen_++;
    if (!on) {
      snap_.clear(); snapBmp_.reset();
      if (isOpen()) { web_.show(true); if (focusBeforeCover_ && parent_ && GetFocus() == parent_) web_.focus(); }   // the keyboard back where the overlay took it from
      focusBeforeCover_ = false; changed(); return;
    }
    focusBeforeCover_ = isOpen() && webFocused_;
    if (webFocused_ && parent_) SetFocus(parent_);       // the overlay is the app's: so are the keys
    if (!isOpen() || web_.state() != web::WebState::Ready) return;
    const unsigned gen = snapGen_; std::shared_ptr<bool> alive = alive_;
    web_.capture([this, alive, gen](bool ok, const std::string& png) {
      if (!*alive || gen != snapGen_ || !covered_) return;
      snap_ = ok ? png : std::string(); snapBmp_.reset();
      web_.show(false);
      changed();
    });
  }
  bool covered() const { return covered_; }
  // the render target is being replaced (device lost, a new window target): the photograph's bitmap goes with it
  void dropDeviceResources() { snapBmp_.reset(); snapRt_ = nullptr; view_.dropDeviceResources(); }
  bool webFocused() const { return webFocused_; }       // the keyboard is in the page (the window is still the active one)
  void focusPage() { if (isOpen() && !covered_) web_.focus(); }   // the keyboard into the page (the window came back to the front)

  // the mouse over the bar (the rest of the pane is the web view's own window; native: the cards). true = the
  // picture changed
  bool mouseMove(float x, float y) {
    const bool h = isOpen() && closeRect_.has(x, y);
    const bool v = native_ && view_.mouseMove(isOpen() ? x : -1, isOpen() ? y : -1);
    if (h == hoverClose_ && !v) return false;
    hoverClose_ = h; return true;
  }
  // true = the click was the pane's (the app does nothing else with it)
  bool click(float x, float y) {
    if (!isOpen() || !area_.has(x, y)) return false;
    if (closeRect_.has(x, y)) closePane();
    else if (native_) view_.click(x, y);
    return true;
  }
  std::wstring tooltipAt(float x, float y) const { return isOpen() && closeRect_.has(x, y) ? L"Close" : native_ && isOpen() ? view_.tooltipAt(x, y) : L""; }
  bool handAt(float x, float y) const { return isOpen() && (closeRect_.has(x, y) || (native_ && view_.hand(x, y))); }
  // the top window moved (WM_MOVE / WM_MOVING): the web view's popups follow
  void parentMoved() { web_.notifyParentMoved(); }
  // before the app exits: the web view closed, its browser process gone, the profile folder removed
  // the first half of shutdown, without waiting: the web view closes, its browser process starts to go
  void beginShutdown() { for (UINT_PTR t : timers_) Timers::cancel(t); timers_.clear(); idleTimer_ = 0; endWeb(); }
  bool shutdown(unsigned waitMs = 3000) {
    for (UINT_PTR t : timers_) { Timers::cancel(t); }
    timers_.clear(); idleTimer_ = 0;
    endWeb();
    bool clean = web_.destroy(waitMs);
    for (auto& r : retired_) { web::WebPane::waitProcessGone(r.pid, 1500); if (!web::WebPane::removeProfileFolder(r.dir)) clean = false; }   // a deck switch's old profile, if it is still there
    retired_.clear();
    return clean;
  }
  size_t retiredProfiles() const { return retired_.size(); }
  // The window is minimised: nobody can see the page. It is hidden and suspended (its renderer stops running script
  // and timers); restored, it shows again where it was.
  void minimised(bool on) {
    if (on == minimised_) return;
    minimised_ = on;
    if (native_) return;                                 // nothing runs while nobody looks: there is nothing to suspend
    if (on) { if (webMade_) { web_.show(false); web_.suspend(); } }
    else if (isOpen()) {
      if (!webMade_) { ensureWeb(); placeWeb(); paint(); wantFocus_ = true; }   // it was closed while nobody could see it: made again, the pages load again
      if (!covered_) web_.show(true);
    }
    armIdle();
  }
  bool isMinimised() const { return minimised_; }

  // called at the end of every meta (49-asks.js paintAsks)
  void paintAsks() {
    if (!showTile() && asksFocused_) asksBlur();
    if (isOpen() || !sec_.empty()) paint();
  }

 private:
  struct Prog { bool has = false; int drafted = 0, answered = 0, later = 0, total = 0; };
  struct Frame {
    std::string id, sid, kind, src, label;
    bool ready = false, legacy = false, locked = false, blocked = false;
    Prog prog; unsigned serial = 0;
    std::map<int, std::function<void(const json*)>> waiters;
  };
  struct Sec { std::string flash; UINT_PTR flashTimer = 0; bool confirm = false, armed = false, doneBusy = false; std::string confirmText; UINT_PTR armTimer = 0; };

  HWND parent_ = nullptr; Deps deps_;
  DeckLink link_; std::wstring origin_ = L"https://asks.claude-deck.invalid";
  std::string khZoom_ = "F6", khAsks_ = "F7";
  json asks_ = json::array(), askLog_ = json::array(), tiles_ = json::array(), accounts_ = json::array(), cfg_ = json::object();
  std::map<std::string, Frame> askFrames_, logFrames_;      // page id -> its frame (waiting / answered, read-only)
  std::map<std::string, Sec> sec_;                          // askSections: session id -> its header's state
  std::string mode_;                                        // askPaneMode
  bool asksFocused_ = false; std::string asksPick_;
  std::set<std::string> recentOpen_, movedSeen_;
  int askReq_ = 0; unsigned serial_ = 0;
  web::WebPane web_; bool webMade_ = false, hostReady_ = false, focusInFrame_ = false, webFocused_ = false;
  ULONGLONG webT0_ = 0, openT0_ = 0; unsigned firstPageMs_ = 0, createCallMs_ = 0, webReadyMs_ = 0, hostReadyMs_ = 0, accelSeen_ = 0, lastAccel_ = 0;
  bool wantFocus_ = false, focusBeforeCover_ = false, minimised_ = false;
  UINT_PTR idleTimer_ = 0; unsigned webCloses_ = 0; bool counted_ = false;
  bool guest_ = false; std::string guestWhy_;
  bool covered_ = false; unsigned snapGen_ = 0; std::string snap_; Com<ID2D1Bitmap> snapBmp_; ID2D1RenderTarget* snapRt_ = nullptr;
  std::string lastRender_, pendingScroll_; bool wantScroll_ = false;
  std::map<std::string, std::wstring> localPages_;          // the in-process deck: page id -> the page file
  RectF area_, closeRect_; float scale_ = 1; bool hoverClose_ = false;
  std::string barTitle_, barHint_;
  std::shared_ptr<bool> alive_;
  std::vector<UINT_PTR> timers_;
  std::wstring family_;
  // native (askcards.hpp): the view, and the pages it is fetching from a remote deck (their answers come back on
  // this thread: a short timer empties the inbox the worker threads fill)
  bool native_ = false, blink_ = true;
  CardsView view_;
  struct Inbox { std::mutex m; std::vector<std::function<void()>> done; };
  std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
  int fetching_ = 0; UINT_PTR pumpT_ = 0;
  void initCards() {
    CardsView::Deps d;
    d.page = [this](const std::string& kind, const std::string& id, const json& m) { pageMsg(kind, id, m); };
    d.legacy = [this](const std::string& kind, const std::string& id) { Frame* fr = frame(kind, id); if (!fr || fr->ready) return; fr->legacy = true; paint(); };
    d.ui = [this](const std::string& what, const std::string& sid, const std::string& id) { ui(what, sid, id); };
    d.fetch = [this](const std::string& id, std::function<void(bool, const std::string&, const std::string&)> cb) { fetchPage(id, cb); };
    d.focusEdit = [this](EditBox* e) { if (deps_.focusEdit) deps_.focusEdit(e); };
    d.focused = [this]() -> EditBox* { return deps_.focusedEdit ? deps_.focusedEdit() : nullptr; };
    d.openLink = [this](const std::wstring& url) { linkClicked(url); };
    d.copy = [this](const std::string& t) { if (deps_.writeClipboard) deps_.writeClipboard(t); };
    d.changed = [this] { changed(); };
    d.after = [this](unsigned ms, std::function<void()> f) { return after(ms, f); };
    d.cancel = [this](UINT_PTR id) { cancel(id); };
    d.now = [] { FILETIME ft; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
      return deck::core::askcards::isoTime((int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL); };
    view_.init(d);
  }
  // a page for the native view: the in-process deck's file, or the remote deck's /ask/<id>.html. The answer always
  // comes later, on this thread (as a frame's load does)
  void fetchPage(const std::string& id, std::function<void(bool, const std::string&, const std::string&)> cb) {
    if (!link_.valid()) {
      auto it = localPages_.find(id);
      std::string body, err; bool ok = false;
      if (it == localPages_.end()) err = "the deck names no file for it";
      else {
        HANDLE h = CreateFileW(it->second.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) err = "its file is not there";
        else {
          LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
          if (sz.QuadPart > (32 << 20)) err = "too large";
          else { body.resize((size_t)sz.QuadPart); DWORD got = 0; if (!body.empty()) ReadFile(h, &body[0], (DWORD)body.size(), &got, nullptr); body.resize(got); ok = true; }
          CloseHandle(h);
        }
      }
      after(0, [cb, ok, body, err] { cb(ok, body, err); });
      return;
    }
    const DeckLink l = link_; const std::wstring path = L"/ask/" + widen(enc(id)) + L".html";
    std::shared_ptr<Inbox> box = inbox_;
    fetching_++;
    std::thread([box, l, path, cb] {
      std::string body, err; const bool ok = askPageGet(l, path, body, err);
      std::lock_guard<std::mutex> g(box->m);
      box->done.push_back([cb, ok, body, err] { cb(ok, body, err); });
    }).detach();
    pump();
  }
  void pump() {
    if (pumpT_) return;
    pumpT_ = after(25, [this] {
      pumpT_ = 0;
      std::vector<std::function<void()>> d; { std::lock_guard<std::mutex> g(inbox_->m); d.swap(inbox_->done); }
      fetching_ -= (int)d.size();
      for (auto& f : d) f();
      if (fetching_ > 0) pump();
    });
  }

  // ---- small things
  static const json& nul() { static const json j; return j; }
  static const json& obj(const json& j, const char* k) { if (!j.is_object()) return nul(); auto it = j.find(k); return it == j.end() ? nul() : *it; }
  static json arr(const json& j, const char* k) { const json& v = obj(j, k); return v.is_array() ? v : json::array(); }
  static std::string str(const json& j, const char* k) { const json& v = obj(j, k); return v.is_string() ? v.get<std::string>() : std::string(); }
  static double num(const json& j, const char* k, double d = 0) {
    const json& v = obj(j, k);
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) { try { return std::stod(v.get<std::string>()); } catch (...) { return d; } }   // +m.drafted || 0
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    return d;
  }
  static bool truthy(const json& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return true;
  }
  static bool isFalse(const json& j, const char* k) { const json& v = obj(j, k); return v.is_boolean() && !v.get<bool>(); }
  // a number as the js prints it in a template (`round ${r.round}`); null / missing = '?'
  static std::string show(const json& v) {
    if (v.is_number_integer() || v.is_number_unsigned()) return std::to_string(v.get<long long>());
    if (v.is_number()) { const double d = v.get<double>(); if (d == (long long)d) return std::to_string((long long)d); char b[32]; snprintf(b, sizeof b, "%g", d); return b; }
    if (v.is_string()) return v.get<std::string>();
    return "?";
  }
  static std::string plural(int n) { return n == 1 ? "" : "s"; }
  void note(const std::string& t, int ms = 2500) { if (deps_.note) deps_.note(t, ms); }
  void changed() { if (deps_.changed) deps_.changed(); }
  UINT_PTR after(UINT ms, std::function<void()> f) {
    std::shared_ptr<bool> alive = alive_;
    auto idp = std::make_shared<UINT_PTR>(0);
    *idp = Timers::after(ms, [this, alive, idp, f] {
      if (!*alive) return;
      for (size_t i = 0; i < timers_.size(); i++) if (timers_[i] == *idp) { timers_.erase(timers_.begin() + i); break; }
      f();
    });
    if (*idp) timers_.push_back(*idp);
    return *idp;
  }
  void cancel(UINT_PTR& id) { if (!id) return; Timers::cancel(id); for (size_t i = 0; i < timers_.size(); i++) if (timers_[i] == id) { timers_.erase(timers_.begin() + i); break; } id = 0; }
  const std::wstring& family(Gfx& g) {
    if (!family_.empty()) return family_;
    family_ = L"Segoe UI";
    Com<IDWriteFontCollection> fc;
    if (g.dw && SUCCEEDED(g.dw->GetSystemFontCollection(fc.put())) && fc) { UINT32 i = 0; BOOL has = FALSE; if (SUCCEEDED(fc->FindFamilyName(L"Segoe UI Variable Text", &i, &has)) && has) family_ = L"Segoe UI Variable Text"; }
    return family_;
  }

  // ---- 49-asks.js, the helpers at its head
  const json* askSession(const std::string& sid) const { for (auto& s : asks_) if (str(s, "sessionId") == sid) return &s; return nullptr; }
  const json* askTileOf(const std::string& sid) const { for (auto& t : tiles_) if (str(t, "kind") == "claude" && str(t, "sessionId") == sid) return &t; return nullptr; }
  const json* tileById(const std::string& id) const { if (id.empty()) return nullptr; for (auto& t : tiles_) if (str(t, "id") == id) return &t; return nullptr; }
  static int askOpenCount(const json& s) { int n = 0; for (auto& r : arr(s, "rounds")) n += std::max(0, (int)num(r, "total") - (int)num(r, "answered")); return n; }
  const Prog* frameProg(const std::string& id) const { auto it = askFrames_.find(id); return it != askFrames_.end() && it->second.prog.has ? &it->second.prog : nullptr; }
  int askDrafted(const json& s) const {
    int n = 0;
    for (auto& r : arr(s, "rounds")) { if (truthy(obj(r, "sent"))) continue; const Prog* p = frameProg(str(r, "id")); n += std::max((int)num(r, "drafted"), p ? p->drafted : 0); }
    return n;
  }
  static bool askUnsent(const json& s) { for (auto& r : arr(s, "rounds")) if (!truthy(obj(r, "sent"))) return true; return false; }
  const json* askLastLog(const std::string& sid) const { for (auto& e : askLog_) if (str(e, "sessionId") == sid) return &e; return nullptr; }
  static std::string tileTitle(const json& t) {   // 30-tiles.js tileTitle (a claude tile)
    const std::string ti = str(t, "title"); if (!ti.empty()) return ti;
    const std::string k = str(t, "kind"); return k == "cloud" ? "New cloud session" : k == "shell" ? "Shell" : "New session";
  }
  std::string askName(const json& s) const { const json* t = askTileOf(str(s, "sessionId")); if (t) return tileTitle(*t); const std::string ti = str(s, "title"); return ti.empty() ? str(s, "name") : ti; }
  std::string abbrOfTile(const json* t) const {
    if (!t) return "";
    const std::string a = str(*t, "account");
    for (auto& ac : accounts_) if (str(ac, "id") == a) return str(ac, "abbr");
    return "";
  }
  std::string askAbbr(const json& s) const { return abbrOfTile(askTileOf(str(s, "sessionId"))); }
  std::string askLogName(const json& e) const { const json* t = askTileOf(str(e, "sessionId")); if (t) return tileTitle(*t); const std::string ti = str(e, "title"); return ti.empty() ? str(e, "name") : ti; }
  bool askVisible(const std::string& sid) const { return mode_ == "all" || mode_ == sid; }

  // askTime: new Date(v) from a number of ms or an ISO string -> "7:35 PM", plus " Nov 10" when it is not today
  static bool toMs(const json& v, double& ms) {
    if (v.is_number()) { ms = v.get<double>(); return true; }
    if (!v.is_string()) return false;
    const std::string s = v.get<std::string>();
    int Y, M, D, h = 0, mi = 0; double sec = 0; int used = 0;
    if (sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%lf%n", &Y, &M, &D, &h, &mi, &sec, &used) < 5) return false;
    SYSTEMTIME st = {}; st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D; st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)sec;
    FILETIME ft; if (!SystemTimeToFileTime(&st, &ft)) return false;
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    ms = (double)((long long)(u.QuadPart / 10000ULL) - 11644473600000LL) + (sec - (int)sec) * 1000.0;
    const char* z = s.c_str() + (used > 0 ? used : (int)s.size());
    if (*z == '+' || *z == '-') { int oh = 0, om = 0; if (sscanf(z + 1, "%2d:%2d", &oh, &om) >= 1 || sscanf(z + 1, "%2d%2d", &oh, &om) >= 1) ms -= (*z == '+' ? 1 : -1) * (oh * 60 + om) * 60000.0; }
    else if (*z != 'Z' && *z != 'z') {     // no zone: local time
      SYSTEMTIME utc; if (TzSpecificLocalTimeToSystemTime(nullptr, &st, &utc) && SystemTimeToFileTime(&utc, &ft)) { u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; ms = (double)((long long)(u.QuadPart / 10000ULL) - 11644473600000LL); }
    }
    return true;
  }
  static std::string askTime(const json& v) {
    double ms; if (!toMs(v, ms)) return "";
    ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)((long long)ms + 11644473600000LL) * 10000ULL;
    FILETIME ft; ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME utc, loc, now; if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) return "";
    GetLocalTime(&now);
    wchar_t t[64] = L"", d[64] = L"";
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &loc, nullptr, t, 64);
    std::wstring out = t;
    if (loc.wYear != now.wYear || loc.wMonth != now.wMonth || loc.wDay != now.wDay) { GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &loc, L"MMM d", d, 64, nullptr); out += L" "; out += d; }
    return narrow(out);
  }

  // ---- the web view
  static std::wstring originOf(const DeckLink& l) {
    std::wstring o = (l.secure ? L"https://" : L"http://") + l.host;
    if (!((l.secure && l.port == 443) || (!l.secure && l.port == 80))) o += L":" + std::to_wstring(l.port);
    return o;
  }
  std::wstring hostUrl() const { return origin_ + L"/__claude-deck-native/asks"; }
  static std::string enc(const std::string& s) {   // encodeURIComponent
    static const char* hex = "0123456789ABCDEF"; std::string o;
    for (unsigned char c : s) { if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') o += (char)c; else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; } }
    return o;
  }
  // askSrc: the page as the browser desk frames it (lib/phone.js serves it; the cookie carries the token)
  std::string askSrc(const std::string& id) const { return narrow(origin_) + "/ask/" + enc(id) + ".html?embed=deck&theme=dark"; }
  // The old deck's web view goes at once; its private profile folder (the old deck's cookie was only ever in memory,
  // but the folder is ours to clean) is removed as soon as its browser process has gone: looked at every 300 ms, six
  // seconds at most, and once more at shutdown. The next web view gets a folder of its own.
  struct Retired { std::wstring dir; unsigned long pid; };
  std::vector<Retired> retired_;
  void retire(int tries = 0) {
    for (size_t i = 0; i < retired_.size();) { if (web::WebPane::waitProcessGone(retired_[i].pid, 0) && web::WebPane::removeProfileFolder(retired_[i].dir)) retired_.erase(retired_.begin() + i); else i++; }
    if (!retired_.empty() && tries < 20) after(300, [this, tries] { retire(tries + 1); });
  }
  // The web view closed, the pane's own state kept (what is open, the sections, the drafts are the deck's). With the
  // last web view of the process the browser process tree goes by itself. The deck's cookie is deleted first: the
  // InPrivate profile dies with its last view anyway, but the browser process may live on for a cloud pane.
  void dropWeb() {
    cancel(idleTimer_);
    if (!webMade_) return;
    // (the windows of one process share the InPrivate profile: the cookie goes with the LAST ask view, not before)
    if (counted_) { counted_ = false; if (--liveWebs() == 0) web_.clearCookies(); }
    { const std::wstring dir = web_.userDataDir(); const unsigned long pid = web_.browserPid(); const bool own = web_.ownsUserData();
      web_.destroy(0);
      if (own && !dir.empty()) { retired_.push_back({ dir, pid }); after(300, [this] { retire(); }); } }   // a folder of the web pane's own making (no shared folder was given): removed once its browser has gone
    webMade_ = false; hostReady_ = false; webFocused_ = false; focusInFrame_ = false; lastRender_.clear();
    snap_.clear(); snapBmp_.reset(); snapGen_++;
    for (auto& kv : askFrames_) for (auto& w : kv.second.waiters) w.second(nullptr);
    askFrames_.clear(); logFrames_.clear();
  }
  // the app is going: the view closes (its cookie first, when it is the last ask view of the process)
  void endWeb() {
    if (counted_) { counted_ = false; if (--liveWebs() == 0) web_.clearCookies(); }
    web_.destroy(0);
  }
  static int& liveWebs() { static int n = 0; return n; }   // ask web views up in this process (every window's)
  void resetWeb() {
    if (!webMade_) return;
    dropWeb();
    if (isOpen() && !minimised_) { ensureWeb(); web_.show(!covered_); paint(); }
    armIdle();
  }
  // Nobody can see the web view (the pane is closed, or open in a minimised window): it is closed after the grace.
  void armIdle() {
    cancel(idleTimer_);
    if (!webMade_ || (isOpen() && !minimised_)) return;
    idleTimer_ = after(isOpen() ? offscreenMs : graceMs, [this] {
      idleTimer_ = 0;
      if (!webMade_ || (isOpen() && !minimised_)) return;
      dropWeb(); webCloses_++;
      changed();
    });
  }
  void placeWeb() {
    if (!webMade_) return;
    const RectF r = webRect();
    web_.setBounds(web::Rect{ r.x, r.y, r.w, r.h }, scale_);
  }
  void ensureWeb() {
    if (native_) { hostReady_ = true; return; }          // native: nothing to make (a headless --shot run draws the cards too)
    if (webMade_ || !parent_) return;                   // no window (a headless --shot run): the bar alone is drawn
    webMade_ = true; hostReady_ = false; focusInFrame_ = false; webFocused_ = false;
    std::shared_ptr<bool> alive = alive_;
    web_.onFocus = [this, alive](bool has) { if (*alive) { webFocused_ = has; if (!has) focusInFrame_ = false; changed(); } };
    web_.onReady = [this, alive](bool ok, const std::wstring&) { if (!*alive) return; if (ok) webReadyMs_ = (unsigned)(GetTickCount64() - webT0_); else changed(); };
    web_.onMessage = [this, alive](const std::string& s) { if (*alive) fromHost(s); };
    web_.onNavigate = [this, alive](const std::wstring& url, bool main) {
      if (!*alive || main) return false;                                  // the top document is the host page, nothing else
      return url.rfind(origin_ + L"/ask/ask_", 0) == 0;                    // a frame: an ask page of this deck
    };
    web_.onNewWindow = [this, alive](const std::wstring& url) {            // a link on a page (target=_blank): main.js openAskLink
      if (!*alive) return;
      linkClicked(url);
    };
    web_.onAccelerator = [this, alive](unsigned vk, bool c, bool s, bool a) {
      if (!*alive) return false;
      // whose key it is: keys.hpp webKeyOf (pure, unit-tested). The page keeps everything it types or edits with.
      const WebKey k = webKeyOf(vk, c, s, a, GetAsyncKeyState(VK_RMENU) < 0, focusInFrame_);
      accelSeen_++; lastAccel_ = vk;
      if (k == WebKey::Page) return false;
      if (k == WebKey::ClosePane) { closePane(); return true; }
      if (deps_.accelerator) deps_.accelerator(vk, c, s, a);
      return true;                                       // the app's key: never the page's, whatever the app did with it
    };
    web_.onResource = [this, alive](const std::wstring& url, web::WebResponse& out) { return *alive && localPage(url, out); };
    web_.onDownload = [this, alive](const std::wstring& name) { return *alive && deps_.downloadTo ? deps_.downloadTo(name) : std::wstring(); };
    const RectF r = webRect();
    webT0_ = GetTickCount64(); webReadyMs_ = hostReadyMs_ = 0;
    const bool started = web_.create(parent_, web::Rect{ r.x, r.y, r.w, r.h }, scale_, webOptions);
    createCallMs_ = (unsigned)(GetTickCount64() - webT0_);
    if (!started) return;                                // no runtime: draw() says so
    liveWebs()++; counted_ = true;
    web::WebResponse doc;
    doc.headers = L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
      L"Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-src " + origin_ + L"/ask/; base-uri 'none'; form-action 'none'";
    doc.body = hostDocument();
    web_.serve(hostUrl(), doc);
    if (link_.valid()) {
      // lib/phone.js: `deck_phone=<encodeURIComponent(token)>; Path=/; SameSite=Lax; HttpOnly`, in the InPrivate profile
      web_.setCookie(L"deck_phone", widen(enc(link_.token)), link_.host, L"/", link_.secure);
    } else web_.addResourceFilter(origin_ + L"/ask/*");
    web_.navigate(hostUrl());
  }
  // the in-process deck has no /ask/ route: the page file itself, with the header lib/phone.js serves it under
  bool localPage(const std::wstring& url, web::WebResponse& out) {
    if (link_.valid()) return false;
    const std::string u = narrow(url), pre = narrow(origin_) + "/ask/";
    if (u.rfind(pre, 0) != 0) return false;
    std::string id = u.substr(pre.size()); const size_t q = id.find_first_of("?#"); if (q != std::string::npos) id = id.substr(0, q);
    if (id.size() < 6 || id.substr(id.size() - 5) != ".html") { out.status = 404; return true; }
    id = id.substr(0, id.size() - 5);
    auto it = localPages_.find(id);
    out.headers = L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\n"
      L"Content-Security-Policy: sandbox allow-scripts allow-popups allow-popups-to-escape-sandbox allow-downloads; default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:";
    if (it == localPages_.end()) { out.status = 404; out.body = "no such ask page"; return true; }
    HANDLE h = CreateFileW(it->second.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { out.status = 404; out.body = "not found"; return true; }
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (sz.QuadPart > (32 << 20)) { CloseHandle(h); out.status = 404; return true; }
    out.body.resize((size_t)sz.QuadPart); DWORD got = 0;
    if (!out.body.empty()) ReadFile(h, &out.body[0], (DWORD)out.body.size(), &got, nullptr);
    out.body.resize(got); CloseHandle(h);
    return true;
  }
  // meta's file:///C:/…/ask_….html -> the path, only for a file named like an ask page
  void rememberLocal(const std::string& id, const std::string& url) {
    if (link_.valid() || url.rfind("file:///", 0) != 0) return;
    std::string p;
    for (size_t i = 8; i < url.size(); i++) {
      if (url[i] == '%' && i + 2 < url.size()) { p += (char)strtol(url.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
      else p += url[i] == '/' ? '\\' : url[i];
    }
    const size_t sl = p.find_last_of('\\'); const std::string base = sl == std::string::npos ? p : p.substr(sl + 1);
    if (base.rfind("ask_", 0) != 0 || base.size() < 9 || base.substr(base.size() - 5) != ".html") return;
    localPages_[id] = widen(p);
  }
  void toHost(const json& m) {
    if (native_) {                                        // the view stands in for the host document and its frames
      const std::string op = str(m, "op");
      if (op == "post") view_.post(str(m, "kind"), str(m, "id"), obj(m, "msg"));
      else if (op == "scroll") view_.scrollTo(str(m, "to"));
      else if (op == "render") view_.render(m);
      return;
    }
    if (webMade_ && hostReady_) web_.postJson(m.dump(-1, ' ', false, json::error_handler_t::replace));
  }
  void flushScroll() {
    if (!wantScroll_ || !hostReady_) return;
    wantScroll_ = false;
    toHost(json{ { "op", "scroll" }, { "to", pendingScroll_ } });
  }

  // ---- frames (makeFrame / dropFrame): the host document makes the elements, this is their state
  Frame& makeFrame(std::map<std::string, Frame>& map, const char* kind, const std::string& id, const std::string& sid, const std::string& label) {
    auto it = map.find(id);
    if (it != map.end()) return it->second;
    Frame& fr = map[id];
    fr.id = id; fr.sid = sid; fr.kind = kind; fr.label = label; fr.src = askSrc(id); fr.serial = ++serial_;
    if (native_) view_.open(kind, id, sid, label);          // (its page answers later: never inside this call)
    return fr;
  }
  void dropFrame(std::map<std::string, Frame>& map, const std::string& id) {
    auto it = map.find(id); if (it == map.end()) return;
    auto waiters = std::move(it->second.waiters);
    if (native_) view_.drop(it->second.kind, id);
    map.erase(it);
    for (auto& w : waiters) w.second(nullptr);
  }
  Frame* frame(const std::string& kind, const std::string& id) {
    auto& map = kind == "log" ? logFrames_ : askFrames_;
    auto it = map.find(id); return it == map.end() ? nullptr : &it->second;
  }

  // ---- paintAskPane
  void paint() {
    std::set<std::string> live; for (auto& s : asks_) live.insert(str(s, "sessionId"));
    // a session whose pages all went (it read its answers): its section goes. Showing only it, the pane shows its
    // answers read-only instead (F7 until it asks again), or closes when they are not in the log.
    for (auto it = sec_.begin(); it != sec_.end();) {
      if (live.count(it->first)) { ++it; continue; }
      std::vector<std::string> ids; for (auto& kv : askFrames_) if (kv.second.sid == it->first) ids.push_back(kv.first);
      for (auto& id : ids) dropFrame(askFrames_, id);
      cancel(it->second.flashTimer); cancel(it->second.armTimer);
      it = sec_.erase(it);
    }
    if (!mode_.empty() && mode_ != "all" && logModeId().empty() && !live.count(mode_)) {
      const std::string gone = mode_; const json* last = askLastLog(gone);
      // its page was pushed out by another session's newer one: follow it there (their answers came over with it)
      const json* heir = nullptr;
      for (auto& x : asks_) { for (auto& r : arr(x, "rounds")) for (auto& p : arr(r, "replaces")) if (str(p, "sessionId") == gone) heir = &x; if (heir) break; }
      if (heir) {
        int n = 0; for (auto& r : arr(*heir, "rounds")) for (auto& q : arr(r, "replaces")) if (str(q, "sessionId") == gone) n += (int)num(q, "moved");
        mode_ = str(*heir, "sessionId");
        note("That page was replaced by " + askName(*heir) + "'s newer one" + (n ? "; your " + std::to_string(n) + " answer" + plural(n) + " came over" : "") + ".", 6000);
      } else if (last) { mode_ = "log:" + str(*last, "id"); }
      else { const json* t = askTileOf(gone); const std::string who = t ? tileTitle(*t) : "The session"; closePane(); note(who + " has read its answers."); return; }
    }
    if (!isOpen()) { render(); return; }
    const std::string lid = logModeId();
    for (auto& s : asks_) {
      const std::string sid = str(s, "sessionId");
      sec_[sid];                                         // askSection(s)
      const bool vis = askVisible(sid);
      std::set<std::string> want; for (auto& r : arr(s, "rounds")) want.insert(str(r, "id"));
      std::vector<std::string> gone; for (auto& kv : askFrames_) if (kv.second.sid == sid && !want.count(kv.first)) gone.push_back(kv.first);
      for (auto& id : gone) dropFrame(askFrames_, id);
      if (vis) for (auto& r : arr(s, "rounds")) {
        const std::string id = str(r, "id");
        rememberLocal(id, str(r, "url"));
        Frame& fr = makeFrame(askFrames_, "ask", id, sid, str(s, "name") + " round " + show(obj(r, "round")));
        if ((truthy(obj(r, "sent")) || guest_) && fr.ready && !fr.locked) { fr.locked = true; askPost(fr, json{ { "askpage", "lock" } }); askEnsureLocked("ask", id); }
        for (auto& p : arr(r, "replaces")) {
          const int moved = (int)num(p, "moved");
          const std::string key = id + "<" + str(p, "id");
          if (moved && !movedSeen_.count(key)) { movedSeen_.insert(key); askFlash(sid, "A newer page replaced " + whose(p, sid) + "; your " + std::to_string(moved) + " answer" + plural(moved) + " came over.", 8000, false); }
        }
      }
    }
    // paintRecent's frames
    {
      const bool showAll = mode_ == "all" ? !askLog_.empty() : !lid.empty();
      std::set<std::string> want;
      if (showAll) for (auto& e : askLog_) { const std::string id = str(e, "id"); if (!lid.empty() && id != lid) continue; if (!lid.empty() || recentOpen_.count(id)) want.insert(id); }
      std::vector<std::string> gone; for (auto& kv : logFrames_) if (!want.count(kv.first)) gone.push_back(kv.first);
      for (auto& id : gone) dropFrame(logFrames_, id);
      for (auto& e : askLog_) {
        const std::string id = str(e, "id"); if (!want.count(id)) continue;
        const std::string url = str(e, "url");
        rememberLocal(id, url);
        // e.url || !canWebview: a remote always asks the deck for the page; the in-process deck only when the file is there
        if (link_.valid() || !url.empty()) makeFrame(logFrames_, "log", id, str(e, "sessionId"), str(e, "name") + " round " + show(obj(e, "round")) + ", answered");
      }
    }
    if (mode_ == "all") barTitle_ = !asks_.empty() ? "Asks \xC2\xB7 " + std::to_string(asks_.size()) + " session" + plural((int)asks_.size()) + " waiting" : "Asks \xC2\xB7 nothing waiting";
    else if (!lid.empty()) { const json* e = nullptr; for (auto& x : askLog_) if (str(x, "id") == lid) { e = &x; break; } barTitle_ = e ? askLogName(*e) + " \xC2\xB7 answered " + askTime(obj(*e, "at")) + " \xC2\xB7 read-only" : "Answered"; }
    else { const json* s = askSession(mode_); barTitle_ = s ? askName(*s) + " asks" : "Asks"; }
    const std::string key = mode_ == "all" ? khZoom_ + " or " + khAsks_ : khAsks_;
    barHint_ = !lid.empty() ? key + " closes \xC2\xB7 it shows here until the session asks again"
      : guest_ ? key + " closes \xC2\xB7 read-only: " + guestWhy_
      : key + " closes \xC2\xB7 Done sends that session its answers";
    render();
    changed();
  }
  std::string whose(const json& p, const std::string& sid) const {
    const std::string ps = str(p, "sessionId"), rd = show(obj(p, "round"));
    if (!ps.empty() && ps != sid) { const std::string nm = str(p, "name"); return (nm.empty() ? "another session" : nm) + "'s round " + rd; }
    return "round " + rd;
  }

  // What the host document shows, whole: the sections (askSection + paintAskHead + each round's note), the
  // recently answered rows (paintRecent), the empty line. Sent only when it differs from the last one.
  void render() {
    if (!native_ && (!webMade_ || !hostReady_)) return;
    const std::string lid = logModeId();
    json out = { { "op", "render" }, { "sections", json::array() } };
    int i = 0;
    for (auto& s : asks_) {
      const std::string sid = str(s, "sessionId");
      auto si = sec_.find(sid); if (si == sec_.end()) { i++; continue; }
      const Sec& sec = si->second;
      const bool vis = isOpen() && askVisible(sid);
      const json* t = askTileOf(sid);
      const json rounds = arr(s, "rounds");
      const bool ended = truthy(obj(s, "ended"));
      json js = { { "sid", sid }, { "order", i++ }, { "hidden", !vis }, { "abbr", askAbbr(s) }, { "name", askName(s) } };
      const Mark mk = markInfo(sid);
      js["mark"] = mk.has ? json{ { "kind", mk.kind }, { "text", mk.text }, { "title", mk.title } } : json(nullptr);
      // paintAskHead
      const json* last = rounds.empty() ? nullptr : &rounds.back();
      const std::string asked = last && truthy(obj(*last, "created")) ? askTime(obj(*last, "created")) : "";
      std::vector<std::string> meta = { str(s, "name"), str(s, "account"),
        rounds.size() > 1 ? std::to_string(rounds.size()) + " pages" : "round " + (last ? show(obj(*last, "round")) : "?"),
        asked.empty() ? "" : "asked " + asked, ended ? "session ended" : !t ? "outside the deck" : "" };
      js["meta"] = join(meta, " \xC2\xB7 ");
      int total = 0, answered = 0, later = 0;
      for (auto& r : rounds) { const Prog* p = frameProg(str(r, "id")); total += (int)num(r, "total"); answered += p ? p->answered : (int)num(r, "answered"); later += p ? p->later : (int)num(r, "later"); }
      const bool unsent = askUnsent(s);
      js["count"] = std::to_string(answered) + " of " + std::to_string(total) + " answered" + (later ? ", " + std::to_string(later) + " later" : "") + (unsent ? "" : " \xC2\xB7 sent");
      js["done"] = { { "disabled", !unsent || sec.doneBusy || guest_ }, { "text", unsent ? "Done" : "Sent" },
        { "title", guest_ ? guestLine() : !unsent ? "Sent; it shows read-only until the session reads it"
          : ended ? "Its session has ended: save the answers file next to the page; it is read when the session is resumed"
          : t ? "Save the answers file next to the page and tell the session where it is, in one line (after its turn, if it is busy)"
          : "Save the answers file next to the page; the session picks it up" } };
      js["term"] = { { "text", t ? "Answer in the terminal" : "Answer in its terminal" },
        { "title", t ? "Close this and go to the session (" + khAsks_ + " brings it back)" : "It runs outside the deck: close this and answer it there" } };
      js["discardDisabled"] = guest_ || !askDrafted(s);
      js["flash"] = guest_ && sec.flash.empty() ? guestLine() : sec.flash;   // the section's own line of text: plain words, no control
      js["confirm"] = sec.confirm ? json{ { "text", sec.confirmText }, { "armed", sec.armed } } : json(nullptr);
      json jr = json::array(); int k = 0;
      for (auto& r : rounds) {
        const std::string id = str(r, "id");
        auto fi = askFrames_.find(id);
        json one = { { "id", id }, { "order", k }, { "frame", fi != askFrames_.end() } };
        if (fi != askFrames_.end()) {
          const Frame& fr = fi->second;
          one["src"] = fr.src; one["label"] = fr.label; one["legacy"] = fr.legacy; one["blocked"] = fr.blocked;
          const bool several = rounds.size() > 1, picker = str(r, "source") == "picker";
          const json& sent = obj(r, "sent");
          std::vector<std::string> parts;
          if (fr.legacy) parts.push_back(native_ ? "Made by an older ask-page skill, which this pane does not draw, and Done above cannot read it: answer in the session's terminal (or set askPane to \"web\" in config.json for its own page)."
                                                 : "Made by an older ask-page skill, so Done above cannot read it: use the page's own Copy all answers (then paste in the terminal) or its Done (saves the answers file the session watches for).");
          if (several) parts.push_back("Round " + show(obj(r, "round")) + (str(r, "title").empty() ? "" : " \xC2\xB7 " + str(r, "title")));
          for (auto& p : arr(r, "replaces")) { const int moved = (int)num(p, "moved"); parts.push_back("Replaces " + whose(p, sid) + (moved ? ": your " + std::to_string(moved) + " answer" + plural(moved) + " there came over" : "")); }
          if (ended && k == 0) parts.push_back("Its session has ended: Done still saves the answers file, but nothing reads it until the session is resumed or another session takes this page over");
          if (truthy(sent)) parts.push_back("Sent " + askTime(obj(sent, "at")) + (str(sent, "how") == "file" ? " as its answers file" : "") + ": read-only, waiting for the session to read it");
          else if (picker) parts.push_back("From Claude Code's picker: it is also open in the session's terminal, and Done presses Esc there first");
          else if (truthy(obj(r, "background"))) parts.push_back("Also asked in the session's terminal: answer either place");
          one["note"] = join(parts, " \xC2\xB7 ");
        }
        jr.push_back(one); k++;
      }
      js["rounds"] = jr;
      out["sections"].push_back(js);
    }
    // paintRecent
    {
      const bool showR = isOpen() && (mode_ == "all" ? !askLog_.empty() : !lid.empty());
      json R = { { "show", showR }, { "headHidden", !lid.empty() },
        { "meta", "the last " + std::to_string(askLog_.size()) + " you answered, every session \xC2\xB7 read-only" }, { "rows", json::array() } };
      int k = 0;
      if (showR) for (auto& e : askLog_) {
        const std::string id = str(e, "id");
        if (!lid.empty() && id != lid) continue;
        const json lines = arr(e, "lines");
        const json& rb = obj(e, "replacedBy");
        std::vector<std::string> meta = { str(e, "name"), "round " + show(obj(e, "round")), std::to_string(lines.size()) + " answer" + plural((int)lines.size()), str(e, "how") + " " + askTime(obj(e, "at")) };
        if (truthy(rb)) { const std::string rs = str(rb, "sessionId"), rn = str(rb, "name"); meta.push_back("replaced by " + (!rs.empty() && rs != str(e, "sessionId") ? (rn.empty() ? std::string("another session") : rn) + "'s " : std::string()) + "round " + show(obj(rb, "round"))); }
        const bool open = !lid.empty() || recentOpen_.count(id) > 0;
        json row = { { "id", id }, { "sid", str(e, "sessionId") }, { "order", k++ }, { "abbr", abbrOfTile(askTileOf(str(e, "sessionId"))) }, { "name", askLogName(e) }, { "meta", join(meta, " \xC2\xB7 ") }, { "open", open } };
        auto fi = logFrames_.find(id);
        const bool hasFrame = open && fi != logFrames_.end() && !fi->second.legacy;       // a legacy answered page: its frame is removed
        row["frame"] = hasFrame;
        if (hasFrame) { row["src"] = fi->second.src; row["label"] = fi->second.label; row["blocked"] = fi->second.blocked; }
        // no page any more, or one from before the bridge: the answer lines, as they were sent
        if (open && !hasFrame) { std::vector<std::string> ls; for (auto& l : lines) ls.push_back(l.is_string() ? l.get<std::string>() : l.dump()); const std::string txt = join(ls, "\n", true); row["lines"] = txt.empty() ? "(no answers)" : txt; }
        else row["lines"] = nullptr;
        R["rows"].push_back(row);
      }
      out["recent"] = R;
    }
    const bool emptyHidden = !asks_.empty() || !lid.empty() || !askLog_.empty();
    out["empty"] = emptyHidden ? json(nullptr) : json("No questions waiting. Sessions that ask on a page show up here and in the asks tile.");
    std::string s = out.dump(-1, ' ', false, json::error_handler_t::replace);
    if (s == lastRender_) return;
    lastRender_ = s;
    if (native_) { view_.render(out); return; }
    web_.postJson(s);
  }
  static std::string join(const std::vector<std::string>& v, const char* sep, bool keepEmpty = false) {
    std::string o; bool first = true;
    for (auto& s : v) { if (s.empty() && !keepEmpty) continue; if (!first) o += sep; o += s; first = false; }
    return o;
  }
  void askFlash(const std::string& sid, const std::string& text, int ms = 5000, bool draw = true) {
    auto it = sec_.find(sid); if (it == sec_.end()) return;
    it->second.flash = text;
    cancel(it->second.flashTimer);
    it->second.flashTimer = after((UINT)ms, [this, sid] { auto j = sec_.find(sid); if (j == sec_.end()) return; j->second.flashTimer = 0; j->second.flash.clear(); render(); });
    if (draw) render();
  }

  // ---- the bridge to the framed pages (the skill's page.html, "the host bridge")
  void askPost(const Frame& fr, const json& m) { toHost(json{ { "op", "post" }, { "kind", fr.kind }, { "id", fr.id }, { "msg", m } }); }
  void askLoad(const std::string& kind, const std::string& id) {
    Frame* fr = frame(kind, id); if (!fr) return;
    const unsigned serial = fr->serial;
    std::shared_ptr<bool> alive = alive_;
    if (!deps_.call) return;
    if (kind == "log") {
      deps_.call("askLogState", json::array({ id }), [this, alive, id, serial](bool ok, const json& st, const std::string&) {
        if (!*alive) return;
        Frame* f = frame("log", id); if (!f || f->serial != serial) return;
        askPost(*f, json{ { "askpage", "load" }, { "state", ok && st.is_object() ? st : json::object() }, { "readonly", true } });
        askEnsureLocked("log", id);
      });
      return;
    }
    deps_.call("askDraftGet", json::array({ id }), [this, alive, id, serial](bool ok, const json& st, const std::string&) {
      if (!*alive) return;
      Frame* f = frame("ask", id); if (!f || f->serial != serial) return;
      bool sent = guest_;                                  // a guest's pages are read-only: the deck takes no draft
      if (const json* s = askSession(f->sid)) for (auto& r : arr(*s, "rounds")) if (str(r, "id") == id) sent = sent || truthy(obj(r, "sent"));
      askPost(*f, json{ { "askpage", "load" }, { "state", ok && st.is_object() ? st : json::object() }, { "readonly", sent } });   // main's copy is the one every view shares
      if (sent) { f->locked = true; askEnsureLocked("ask", id); }
    });
  }
  // A page built before the skill's read-only lock still takes clicks: one that does not say it is locked gets a
  // cover over its frame, so what was sent stays as it was sent
  void askEnsureLocked(const std::string& kind, const std::string& id) {
    askGet(kind, id, [this, kind, id](const json* m) {
      const json& l = m ? obj(*m, "locked") : nul();
      if (l.is_boolean() && l.get<bool>()) return;
      Frame* f = frame(kind, id); if (!f || f->blocked) return;
      f->blocked = true; render();
    });
  }
  void askGet(const std::string& kind, const std::string& id, std::function<void(const json*)> done, UINT ms = 3000) {
    Frame* fr = frame(kind, id); if (!fr) { done(nullptr); return; }
    const int req = ++askReq_; const unsigned serial = fr->serial;
    auto timer = std::make_shared<UINT_PTR>(0);
    *timer = after(ms, [this, kind, id, req, serial, timer] {
      *timer = 0;
      Frame* f = frame(kind, id); if (!f || f->serial != serial) return;
      auto it = f->waiters.find(req); if (it == f->waiters.end()) return;
      auto w = std::move(it->second); f->waiters.erase(it); w(nullptr);
    });
    fr->waiters[req] = [this, timer, done](const json* m) { cancel(*timer); done(m); };
    askPost(*fr, json{ { "askpage", "get" }, { "req", req } });
  }
  // everything the host document says
  void fromHost(const std::string& text) {
    json m = json::parse(text, nullptr, false);
    if (!m.is_object()) return;
    const std::string ev = str(m, "ev");
    if (ev == "host-ready") { hostReady_ = true; if (!hostReadyMs_) hostReadyMs_ = (unsigned)(GetTickCount64() - webT0_); lastRender_.clear(); render(); flushScroll(); if (isOpen() && !covered_ && wantFocus_) web_.focus(); changed(); return; }
    if (ev == "focus") { focusInFrame_ = truthy(obj(m, "inFrame")); return; }
    if (ev == "legacy") {
      const std::string kind = str(m, "kind"), id = str(m, "id");
      Frame* fr = frame(kind, id); if (!fr || fr->ready) return;
      fr->legacy = true; paint();
      return;
    }
    if (ev == "ui") { ui(str(m, "what"), str(m, "sid"), str(m, "id")); return; }
    if (ev != "page") return;
    pageMsg(str(m, "kind"), str(m, "id"), obj(m, "msg"));
  }
  // a page's message to its host (page.html: ready / change / answers), from its frame or from the native view
  void pageMsg(const std::string& kind, const std::string& id, const json& p) {
    Frame* fr = frame(kind, id);
    if (!fr || !p.is_object() || str(p, "id") != fr->id) return;
    const std::string what = str(p, "askpage");
    auto prog = [&p] { Prog g; g.has = true; g.drafted = (int)num(p, "drafted"); g.answered = (int)num(p, "answered"); g.later = (int)num(p, "later"); g.total = (int)num(p, "total"); return g; };
    if (what == "ready" && !firstPageMs_ && openT0_) firstPageMs_ = (unsigned)(GetTickCount64() - openT0_);
    if (what == "ready") { fr->ready = true; if (kind == "ask") fr->prog = prog(); askLoad(kind, id); }
    else if (what == "change") {
      if (kind != "ask" || fr->locked) return;
      fr->prog = prog();
      const json& st = obj(p, "state");
      if (deps_.fire) deps_.fire("askDraft", json::array({ fr->id, json{ { "state", st.is_object() ? st : json::object() }, { "drafted", fr->prog.drafted }, { "answered", fr->prog.answered }, { "later", fr->prog.later }, { "total", fr->prog.total } } }));
      render(); changed();                         // paintAskHead, the asks tile, the tiles' marks
    } else if (what == "answers") {
      const int req = (int)num(p, "req", -1);
      auto it = fr->waiters.find(req);
      if (it != fr->waiters.end()) { auto w = std::move(it->second); fr->waiters.erase(it); w(&p); }
    }
  }

  // ---- the buttons
  void ui(const std::string& what, const std::string& sid, const std::string& id) {
    if (guest_ && (what == "done" || what == "discard" || what == "discard-yes")) { askFlash(sid, guestLine(), 6000, true); return; }
    if (what == "done") done(sid);
    else if (what == "copy") copy(sid);
    else if (what == "term") toTerminal(sid);
    else if (what == "discard") confirmOn(sid);
    else if (what == "keep") confirmOff(sid);
    else if (what == "discard-yes") { auto it = sec_.find(sid); if (it != sec_.end() && it->second.confirm && it->second.armed) discard(sid); }
    else if (what == "lhead") { if (!logModeId().empty()) return; if (recentOpen_.count(id)) recentOpen_.erase(id); else recentOpen_.insert(id); paint(); }
  }
  // askCollect: every unsent page's answers, one after the other; done(false) = a flash already said why
  void collect(const std::string& sid, std::function<void(bool ok, const json& pages)> done) {
    const json* s = askSession(sid); if (!s) { done(false, json::array()); return; }
    struct St { std::vector<std::string> ids; size_t i = 0; json pages = json::array(); int old = 0; std::function<void()> step; };
    auto st = std::make_shared<St>();
    for (auto& r : arr(*s, "rounds")) if (!truthy(obj(r, "sent"))) st->ids.push_back(str(r, "id"));   // already sent: read-only until the session reads it
    std::shared_ptr<bool> alive = alive_;
    std::weak_ptr<St> weak = st;
    st->step = [this, alive, sid, weak, done] {
      auto st = weak.lock(); if (!st || !*alive) return;
      auto finish = [st, done](bool ok) { done(ok, st->pages); };
      while (st->i < st->ids.size()) {
        const std::string id = st->ids[st->i];
        Frame* fr = frame("ask", id);
        if (fr && fr->legacy) { st->old++; st->i++; continue; }        // an older page: its own buttons answer it
        if (!fr || !fr->ready) { askFlash(sid, "The page is still loading; try again in a moment."); finish(false); return; }
        askGet("ask", id, [this, alive, sid, st, id, finish](const json* m) {
          if (!*alive) return;
          const json& a = m ? obj(*m, "answers") : nul();
          if (!m || !truthy(a)) { askFlash(sid, "The page did not answer; try again."); finish(false); return; }
          st->pages.push_back(json{ { "id", id }, { "answers", a } });
          st->i++;
          if (st->step) st->step();
        });
        return;
      }
      if (st->pages.empty() && st->old) { askFlash(sid, native_ ? "This page was made by an older ask-page skill: answer it in the session's terminal." : "This page was made by an older ask-page skill: use its own Copy all answers or Done.", 8000); finish(false); return; }
      finish(true);
    };
    st->step();       // `st` lives in the waiting askGet's callback between steps
  }
  // the Copy-all text (lib/asks.js composeDone): each page's Copy-all lines, headed by its page id
  static std::string askText(const json& pages) {
    std::vector<std::string> blocks;
    for (auto& p : pages) {
      const json lines = arr(obj(p, "answers"), "lines");
      bool any = false; std::vector<std::string> ls = { str(p, "id") };
      for (auto& l : lines) { const std::string t = l.is_string() ? l.get<std::string>() : l.dump(); if (t.find_first_not_of(" \t\r\n") != std::string::npos) any = true; ls.push_back(t); }
      if (any) blocks.push_back(join(ls, "\n", true));
    }
    return join(blocks, "\n", true);
  }
  static bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }
  void done(const std::string& sid) {
    std::shared_ptr<bool> alive = alive_;
    collect(sid, [this, alive, sid](bool ok, const json& pages) {
      if (!*alive || !ok) return;
      if (pages.empty() || blank(askText(pages))) { askFlash(sid, "Nothing answered yet: pick an option or a reply, or type in a box."); return; }
      auto it = sec_.find(sid); if (it != sec_.end()) { it->second.doneBusy = true; render(); }
      if (!deps_.call) return;
      deps_.call("askDone", json::array({ sid, pages }), [this, alive, sid, pages](bool ok2, const json& res, const std::string& error) {
        if (!*alive) return;
        auto si = sec_.find(sid);
        if (!ok2) {
          // "Error invoking remote method 'ask:done': Error: …" -> the deck's own words
          std::string e = error; const size_t a = e.find("Error invoking remote method '");
          if (a == 0) { size_t b = e.find("': ", 30); if (b != std::string::npos) { e = e.substr(b + 3); if (e.rfind("Error: ", 0) == 0) e = e.substr(7); } }
          if (si != sec_.end()) si->second.doneBusy = false;
          askFlash(sid, e, 8000);
          return;
        }
        for (auto& p : pages) { Frame* fr = frame("ask", str(p, "id")); if (fr) { fr->locked = true; askPost(*fr, json{ { "askpage", "lock" } }); askEnsureLocked("ask", fr->id); } }
        const json* t = askTileOf(sid); const json* s = askSession(sid);
        const bool tileId = truthy(obj(res, "tileId"));
        std::string msg;
        // every Done saves the answers file next to the page; a session in this deck (res.tileId) also gets ONE line naming it
        const std::string status = t ? str(*t, "status") : "";
        if (!tileId && s && truthy(obj(*s, "ended"))) msg = "Saved the answers file for " + str(*s, "name") + ". Its session has ended, so it is read when the session is resumed.";
        else if (!tileId) msg = "Saved the answers file for " + (s ? str(*s, "name") : std::string("it")) + " (it runs outside the deck); it picks it up if it is watching.";
        else if (truthy(obj(res, "esc"))) msg = "Done: saved the answers file, pressed Esc on its picker, then told it where the file is.";
        else if (t && (status == "busy" || status == "waiting" || status == "asleep" || status == "starting")) msg = "Done: saved the answers file; " + tileTitle(*t) + " gets one line naming it as soon as it is back at its prompt.";
        else msg = "Done: saved the answers file, and one line naming it goes in as your next message.";
        // Done closes a session's ask (Settings -> Asks, on by default): F7 shows it again, read-only, until the
        // session asks again. The every-session view stays open while another session still waits.
        bool others = false; for (auto& x : asks_) if (str(x, "sessionId") != sid && askUnsent(x)) others = true;
        const bool close = !isFalse(cfg_, "closeOnDone") && (mode_ == sid || (mode_ == "all" && !others));
        if (si != sec_.end()) si->second.doneBusy = false;
        if (close) { CloseOpts o; if (t && tileId && mode_ == sid) o.toTile = str(*t, "id"); closePane(o); note(msg, 5000); }
        else askFlash(sid, msg, 8000);
      });
    });
  }
  void copy(const std::string& sid) {
    std::shared_ptr<bool> alive = alive_;
    collect(sid, [this, alive, sid](bool ok, const json& pages) {
      if (!*alive || !ok) return;
      const std::string text = askText(pages);
      if (blank(text)) { askFlash(sid, "Nothing answered yet."); return; }
      if (deps_.writeClipboard) deps_.writeClipboard(text);
      int n = 1; for (char c : text) if (c == '\n') n++;
      askFlash(sid, "Copied " + std::to_string(n) + " line" + plural(n) + ": paste them into the session's terminal.");
    });
  }
  void toTerminal(const std::string& sid) { const json* t = askTileOf(sid); CloseOpts o; if (t) o.toTile = str(*t, "id"); closePane(o); }
  void confirmOn(const std::string& sid) {
    auto it = sec_.find(sid); const json* s = askSession(sid); if (it == sec_.end() || !s) return;
    const int n = askDrafted(*s); if (!n) return;
    Sec& sec = it->second;
    sec.confirm = true; sec.armed = false;
    sec.confirmText = "Discard the " + std::to_string(n) + " answer" + plural(n) + " in progress for " + askName(*s) + "? The questions stay; you answer them in the terminal.";
    // hard to misclick: the safe choice is where Done was, and Discard only arms after a moment
    cancel(sec.armTimer);
    sec.armTimer = after(700, [this, sid] { auto j = sec_.find(sid); if (j == sec_.end()) return; j->second.armTimer = 0; j->second.armed = true; render(); });
    render();
  }
  void confirmOffQuiet(Sec& sec) { sec.confirm = false; sec.armed = false; cancel(sec.armTimer); }
  void confirmOff(const std::string& sid) { auto it = sec_.find(sid); if (it == sec_.end()) return; confirmOffQuiet(it->second); render(); }
  void discard(const std::string& sid) {
    const json* s = askSession(sid); if (!s) return;
    for (auto& r : arr(*s, "rounds")) {
      if (truthy(obj(r, "sent"))) continue;
      const std::string id = str(r, "id");
      if (Frame* fr = frame("ask", id)) { askPost(*fr, json{ { "askpage", "clear" } }); }
      if (deps_.call) deps_.call("askClear", json::array({ id }), [](bool, const json&, const std::string&) {});
    }
    confirmOff(sid);
    toTerminal(sid);                    // the same as opening it, entering nothing, and answering in the terminal
  }
};

} }  // namespace deck::asks
