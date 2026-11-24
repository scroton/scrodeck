// windows/src/edit.hpp — the one text field of the native shell, drawn with Direct2D.
//
// The Electron deck's text boxes are <input> / <textarea> elements: the rename box (30-tiles.js), the search
// pane (46-search.js), the palette (88-palette.js), the context menu's inline prompt (35-groups.js
// promptInline), the Past filter (47-past.js), the special-prompt form (30-tiles.js renderHoldForm) and the
// settings fields (50-settings.js). This is what stands in for all of them: a caret, a selection, the
// clipboard, Home / End, word steps, double-click a word, drag to select. `multiline` is the textarea.
// Typed characters arrive as WM_CHAR, which covers every keyboard layout; an input method composes in Windows' own
// composition window, which the app places at the caret (App::placeIme), and its committed text is WM_CHAR too.
//
// It owns no window and no focus: the app decides which box has the keys (ui::State::focusEdit) and hands it
// key presses and characters; whoever draws it calls draw() with the box's rectangle, which is also what the
// mouse functions answer from.
#pragma once
#include "gfx.hpp"
#include "keys.hpp"
#include <algorithm>
#include <functional>
#include <cmath>
#include <cwctype>
#include <string>

namespace deck {

// The clipboard is one lock for the whole desktop: another program (a clipboard manager, a remote-desktop client)
// may be holding it for a few milliseconds at the very moment a copy or a paste is asked for. A few tries, as
// every Windows program makes; a copy or paste that still cannot get it does nothing (and is not half done).
inline bool clipboardOpen(HWND hwnd) {
  for (int i = 0; i < 8; i++) { if (OpenClipboard(hwnd)) return true; Sleep(8); }
  return false;
}
// A test run never touches the real clipboard (other programs — and other sessions on this box — are using it): it
// copies to and pastes from a string of its own. app.cpp points this at one for every --shot / --window-shot run.
inline std::wstring*& testClipboard() { static std::wstring* p = nullptr; return p; }
inline std::wstring clipboardGet(HWND hwnd) {
  if (testClipboard()) return *testClipboard();
  std::wstring out;
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !clipboardOpen(hwnd)) return out;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const wchar_t* p = (const wchar_t*)GlobalLock(h);
    if (p) { const size_t cap = GlobalSize(h) / sizeof(wchar_t); size_t n = 0; while (n < cap && p[n]) n++; out.assign(p, n); GlobalUnlock(h); }   // never past the block, terminated or not
  }
  CloseClipboard(); return out;
}
inline void clipboardSet(HWND hwnd, const std::wstring& s) {
  if (testClipboard()) { *testClipboard() = s; return; }
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
  if (!g) return;
  void* mem = GlobalLock(g);
  if (!mem) { GlobalFree(g); return; }
  memcpy(mem, s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g);
  if (!clipboardOpen(hwnd)) { GlobalFree(g); return; }
  EmptyClipboard();
  if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);   // taken: the system owns the block now; refused: it is still ours
  CloseClipboard();
}

struct EditBox {
  std::wstring text, placeholder;
  int caret = 0, anchor = 0;            // UTF-16 offsets; the selection is between them
  float scrollX = 0, scrollY = 0;
  bool multiline = false;               // a textarea: wraps, Enter is a new line, Up / Down move by line
  float lineHeight = 0;                 // css line-height in px (the cloud dialog's boxes: 13px/1.4 = 18.2); 0 = the font's own
  int seenCaret = -1; size_t seenLen = 0; float seenW = -1;   // a textarea follows its caret when the caret or the text moved, not on every paint (the wheel scrolls it)
  bool readOnly = false;                // selectable and copyable, never changed (the phone link)
  std::string owner;                    // who answers Enter / Escape / a change (the app's editKey)
  std::function<void()> onChange;
  // where it was last drawn: the mouse functions and the caret's scroll answer from these
  RectF rect; Font font; Gfx* g = nullptr;

  bool hasSel() const { return caret != anchor; }
  int selA() const { return std::min(caret, anchor); }
  int selB() const { return std::max(caret, anchor); }
  void set(const std::wstring& s) { text = s; caret = anchor = (int)s.size(); scrollX = scrollY = 0; }
  void selectAll() { anchor = 0; caret = (int)text.size(); }
  std::wstring selected() const { return text.substr(selA(), selB() - selA()); }
  void changed() { if (onChange) onChange(); }

  bool eraseSel() { if (!hasSel()) return false; int a = selA(); text.erase(a, selB() - a); caret = anchor = a; return true; }
  void insert(const std::wstring& s0) {
    if (readOnly) return;
    std::wstring s;
    for (wchar_t c : s0) { if (c == L'\r') continue; if (c == L'\n' && !multiline) { s.push_back(L' '); continue; } if (c == L'\t') { s.push_back(L' '); continue; } if (c < 0x20 && c != L'\n') continue; s.push_back(c); }
    eraseSel();
    text.insert(caret, s); caret += (int)s.size(); anchor = caret;
    changed();
  }
  static bool wordCh(wchar_t c) { return iswalnum(c) || c == L'_' || c > 0x7f; }
  int wordLeft(int i) const { while (i > 0 && !wordCh(text[i - 1])) i--; while (i > 0 && wordCh(text[i - 1])) i--; return i; }
  int wordRight(int i) const { int n = (int)text.size(); while (i < n && wordCh(text[i])) i++; while (i < n && !wordCh(text[i])) i++; return i; }
  int stepLeft(int i) const { if (i <= 0) return 0; i--; if (i > 0 && text[i] >= 0xDC00 && text[i] <= 0xDFFF) i--; return i; }
  int stepRight(int i) const { int n = (int)text.size(); if (i >= n) return n; i++; if (i < n && text[i] >= 0xDC00 && text[i] <= 0xDFFF) i++; return i; }

  Com<IDWriteTextLayout> lay() const {
    Com<IDWriteTextLayout> l;
    if (!g) return l;
    l = g->layout(text.empty() ? std::wstring(L"") : text, font, multiline ? std::max(1.f, rect.w) : 100000.f, 100000.f, multiline, false, 0, lineHeight);
    if (l && multiline) l->SetWordWrapping((DWRITE_WORD_WRAPPING)2 /*EMERGENCY_BREAK*/);
    return l;
  }
  float lineH() const {
    if (lineHeight > 0) return lineHeight;
    if (!g) return font.size * 1.3f;
    Com<IDWriteTextLayout> l = g->layout(L"Ag", font, 1000, 1000, false, false, 0);
    DWRITE_TEXT_METRICS m{}; if (l) l->GetMetrics(&m);
    return m.height > 0 ? m.height : font.size * 1.3f;
  }
  // the caret's place inside the text, layout coordinates
  void caretXY(float& x, float& y, float& h) const {
    x = y = 0; h = lineH();
    Com<IDWriteTextLayout> l = lay(); if (!l) return;
    DWRITE_HIT_TEST_METRICS hm{}; FLOAT fx = 0, fy = 0;
    if (SUCCEEDED(l->HitTestTextPosition((UINT32)caret, FALSE, &fx, &fy, &hm))) { x = fx; y = fy; if (hm.height > 0) h = hm.height; }
  }
  float originY() const { return multiline ? rect.y - scrollY : rect.y + std::floor((rect.h - lineH()) / 2 + 0.5f); }
  int posAt(float px, float py) const {
    Com<IDWriteTextLayout> l = lay(); if (!l) return 0;
    BOOL trailing = FALSE, inside = FALSE; DWRITE_HIT_TEST_METRICS hm{};
    float ly = multiline ? py - originY() : lineH() / 2;
    l->HitTestPoint(px - rect.x + scrollX, ly, &trailing, &inside, &hm);
    int p = (int)hm.textPosition + (trailing ? (int)hm.length : 0);
    return std::max(0, std::min((int)text.size(), p));
  }
  // the text's full height (a textarea: what the wheel can scroll through)
  float contentH() const {
    Com<IDWriteTextLayout> l = lay(); if (!l) return lineH();
    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m);
    float h = std::max(m.height, lineH());
    if (!text.empty() && text.back() == L'\n') h = std::max(h, m.lineCount * lineH());   // the empty last line counts
    return h;
  }
  // the wheel over a textarea: the text scrolls, the caret stays where it is in the text
  void scrollBy(float dy) { if (!multiline) return; scrollY = std::max(0.f, std::min(std::max(0.f, contentH() - rect.h), scrollY + dy)); }
  void keepCaretVisible() {
    float x, y, h; caretXY(x, y, h);
    if (multiline) {
      if (y < scrollY) scrollY = y; else if (y + h > scrollY + rect.h) scrollY = y + h - rect.h;
      if (scrollY < 0) scrollY = 0;
      scrollX = 0;
    } else {
      if (x - scrollX > rect.w - 1) scrollX = x - rect.w + 1;
      if (x - scrollX < 0) scrollX = x;
      if (scrollX < 0) scrollX = 0;
    }
  }

  // A key press. true = taken (the caller drops its WM_CHAR); false = the owner's (Enter, Escape, Tab, F-keys).
  bool key(UINT vk, const Mods& m, HWND hwnd) {
    auto move = [&](int to) { caret = std::max(0, std::min((int)text.size(), to)); if (!m.shift) anchor = caret; keepCaretVisible(); };
    switch (vk) {
      case VK_LEFT: if (!m.shift && hasSel() && !m.ctrl) move(selA()); else move(m.ctrl ? wordLeft(caret) : stepLeft(caret)); return true;
      case VK_RIGHT: if (!m.shift && hasSel() && !m.ctrl) move(selB()); else move(m.ctrl ? wordRight(caret) : stepRight(caret)); return true;
      case VK_HOME: case VK_END: {
        if (!multiline || m.ctrl) { move(vk == VK_HOME ? 0 : (int)text.size()); return true; }
        float x, y, h; caretXY(x, y, h);
        Com<IDWriteTextLayout> l = lay(); if (!l) return true;
        BOOL tr = FALSE, in = FALSE; DWRITE_HIT_TEST_METRICS hm{};
        l->HitTestPoint(vk == VK_HOME ? 0.f : 99999.f, y + h / 2, &tr, &in, &hm);
        int p = (int)hm.textPosition + (tr ? (int)hm.length : 0);
        if (vk == VK_END && p > 0 && p <= (int)text.size() && text[p - 1] == L'\n') p--;
        move(p); return true;
      }
      case VK_UP: case VK_DOWN: {
        if (!multiline) return false;
        float x, y, h; caretXY(x, y, h);
        Com<IDWriteTextLayout> l = lay(); if (!l) return true;
        float ty = y + h / 2 + (vk == VK_UP ? -h : h);
        if (ty < 0) { move(0); return true; }
        DWRITE_TEXT_METRICS tm{}; l->GetMetrics(&tm);
        if (ty > tm.height) { move((int)text.size()); return true; }
        BOOL tr = FALSE, in = FALSE; DWRITE_HIT_TEST_METRICS hm{};
        l->HitTestPoint(x, ty, &tr, &in, &hm);
        move((int)hm.textPosition + (tr ? (int)hm.length : 0)); return true;
      }
      case VK_BACK:
        if (readOnly) return true;
        if (!eraseSel() && caret > 0) { int a = m.ctrl ? wordLeft(caret) : stepLeft(caret); text.erase(a, caret - a); caret = anchor = a; }
        keepCaretVisible(); changed(); return true;
      case VK_DELETE:
        if (readOnly) return true;
        if (m.shift && hasSel()) { clipboardSet(hwnd, selected()); eraseSel(); }
        else if (!eraseSel() && caret < (int)text.size()) { int b = m.ctrl ? wordRight(caret) : stepRight(caret); text.erase(caret, b - caret); }
        keepCaretVisible(); changed(); return true;
      case VK_RETURN:
        if (!multiline || m.ctrl) return false;
        insert(L"\n"); keepCaretVisible(); return true;
      case VK_INSERT:
        if (m.ctrl && hasSel()) { clipboardSet(hwnd, selected()); return true; }
        if (m.shift) { insert(clipboardGet(hwnd)); keepCaretVisible(); return true; }
        return false;
      default: break;
    }
    if (m.ctrl && !m.alt) {
      if (vk == 'A') { selectAll(); return true; }
      if (vk == 'C') { if (hasSel()) clipboardSet(hwnd, selected()); return true; }
      if (vk == 'X') { if (hasSel()) { clipboardSet(hwnd, selected()); if (!readOnly) { eraseSel(); changed(); } } keepCaretVisible(); return true; }
      if (vk == 'V') { insert(clipboardGet(hwnd)); keepCaretVisible(); return true; }
    }
    return false;
  }
  // typed text (WM_CHAR); control characters are the keys above
  void ch(wchar_t c) {
    if (c < 0x20 || c == 0x7f) return;
    insert(std::wstring(1, c)); keepCaretVisible();
  }
  // clicks: 1 = place the caret (Shift extends), 2 = the word, 3 = everything
  void mouseDown(float x, float y, bool shift, int clicks) {
    int p = posAt(x, y);
    if (clicks >= 3) { selectAll(); return; }
    if (clicks == 2) { int a = p, b = p; while (a > 0 && wordCh(text[a - 1])) a--; while (b < (int)text.size() && wordCh(text[b])) b++; anchor = a; caret = b; return; }
    caret = p; if (!shift) anchor = p;
  }
  void mouseDrag(float x, float y) { caret = posAt(x, y); keepCaretVisible(); }

  // Draws the TEXT of the field into `r` (the content box: the caller paints the border and the background).
  // css: an empty field shows its placeholder; a focused one its caret (blinking) and selection.
  void draw(Gfx& gfx, const RectF& r, const Font& f, const Rgba& fg, const Rgba& ph, bool focused, bool blinkOn) {
    g = &gfx; rect = r; font = f;
    if (r.w <= 0 || r.h <= 0) return;
    caret = std::max(0, std::min((int)text.size(), caret)); anchor = std::max(0, std::min((int)text.size(), anchor));
    if (multiline) {
      if (focused && (caret != seenCaret || text.size() != seenLen || r.w != seenW)) keepCaretVisible();
      seenCaret = caret; seenLen = text.size(); seenW = r.w;
      scrollY = std::max(0.f, std::min(std::max(0.f, contentH() - r.h), scrollY));
    }
    else if (focused) keepCaretVisible(); else if (!hasSel()) scrollX = 0;
    gfx.pushClip(RectF(r.x - 1, r.y, r.w + 2, r.h));
    const float oy = originY(), ox = r.x - scrollX;
    if (text.empty()) {
      if (!placeholder.empty()) {
        Com<IDWriteTextLayout> pl = gfx.layout(placeholder, f, multiline ? r.w : 100000.f, 100000.f, multiline, false, 0, lineHeight);
        if (pl) gfx.rt->DrawTextLayout(D2D1::Point2F(r.x, oy), pl.p, gfx.b(ph), (D2D1_DRAW_TEXT_OPTIONS)4);
      }
    } else {
      Com<IDWriteTextLayout> l = lay();
      if (l) {
        if (hasSel() && focused) {
          DWRITE_HIT_TEST_METRICS hm[64]; UINT32 n = 0;
          if (SUCCEEDED(l->HitTestTextRange((UINT32)selA(), (UINT32)(selB() - selA()), 0, 0, hm, 64, &n)))
            for (UINT32 i = 0; i < n; i++) gfx.fill(RectF(ox + hm[i].left, oy + hm[i].top, std::max(2.f, hm[i].width), hm[i].height), Rgba::hex(0x0078d7));
        }
        gfx.rt->DrawTextLayout(D2D1::Point2F(ox, oy), l.p, gfx.b(fg), (D2D1_DRAW_TEXT_OPTIONS)4);
      }
    }
    if (focused && blinkOn && !hasSel()) {
      float cx, cy, chh; caretXY(cx, cy, chh);
      gfx.fill(RectF(std::floor(ox + cx + 0.5f), oy + cy, 1, chh), fg);
    }
    gfx.popClip();
  }
};

}  // namespace deck
