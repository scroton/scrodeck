// core/vt/buffer.cpp — line operations, the scrollback buffer, resize and reflow.
// Ports of @xterm/xterm src/common/buffer/BufferLine.ts, Buffer.ts and BufferReflow.ts (6.0.0), statement for
// statement where behaviour is visible. JS-isms that matter are kept: reads past a line's end yield an empty
// zero-width cell, writes past the end are dropped.
#include "vt_internal.hpp"

#include <algorithm>
#include <cstddef>

namespace deck { namespace vt { namespace detail {

Cell toCell(const Attr& a) {
  Cell c;
  c.ch = 0; c.width = 1; c.extra = 0;
  uint16_t f = 0;
  const bool hasExt = (a.bg & BG_HAS_EXT) != 0;
  const int ulEff = a.ulEffective();
  if (a.fg & FG_BOLD) f |= Bold;
  if (a.bg & BG_DIM) f |= Dim;
  if (a.bg & BG_ITALIC) f |= Italic;
  if ((hasExt && ulEff != 0) || (a.fg & FG_UNDERLINE)) f |= Underline;       // AttributeData.isUnderline()
  if (a.fg & FG_BLINK) f |= Blink;
  if (a.fg & FG_INVERSE) f |= Inverse;
  if (a.fg & FG_INVISIBLE) f |= Invisible;
  if (a.fg & FG_STRIKE) f |= Strike;
  if (a.bg & BG_OVERLINE) f |= Overline;
  const int style = hasExt ? ulEff : ((a.fg & FG_UNDERLINE) ? 1 : 0);
  f |= (uint16_t)((style & 7) << UnderlineStyleShift);
  if (a.bg & BG_PROTECTED) f |= Protected;
  if (hasExt && a.url) f |= Hyperlink;
  c.flags = f;
  auto col = [](uint32_t v) {
    Color k;
    switch (v & CM_MASK) {
      case CM_P16: case CM_P256: k.kind = Color::Palette; k.value = v & PCOLOR_MASK; break;
      case CM_RGB: k.kind = Color::RGB; k.value = v & RGB_MASK; break;
      default: k.kind = Color::Default; k.value = 0;
    }
    return k;
  };
  c.fg = col(a.fg);
  c.bg = col(a.bg);
  return c;
}

// ------------------------------------------------------------------------------------------------ BufferLine

void insertCells(Line& l, int pos, int n, const Cell& fill) {
  const int len = lineLen(l);
  if (len == 0) return;
  pos %= len;
  // handle fullwidth at pos: reset cell one to the left if pos is second cell of a wide char
  if (pos && getWidth(l, pos - 1) == 2) setCellCp(l, pos - 1, 0, 1, fill);
  if (n < len - pos) {
    for (int i = len - pos - n - 1; i >= 0; --i) l.cells[pos + n + i] = l.cells[pos + i];
    for (int i = 0; i < n; ++i) l.cells[pos + i] = fill;
  } else {
    for (int i = pos; i < len; ++i) l.cells[i] = fill;
  }
  // handle fullwidth at line end: reset last cell if it is first cell of a wide char
  if (getWidth(l, len - 1) == 2) setCellCp(l, len - 1, 0, 1, fill);
}

void deleteCells(Line& l, int pos, int n, const Cell& fill) {
  const int len = lineLen(l);
  if (len == 0) return;
  pos %= len;
  if (n < len - pos) {
    for (int i = 0; i < len - pos - n; ++i) l.cells[pos + i] = l.cells[pos + n + i];
    for (int i = len - n; i < len; ++i) l.cells[i] = fill;
  } else {
    for (int i = pos; i < len; ++i) l.cells[i] = fill;
  }
  // handle fullwidth at pos:
  // - reset pos-1 if wide char
  // - reset pos if width==0 (previous second cell of a wide char)
  if (pos && getWidth(l, pos - 1) == 2) setCellCp(l, pos - 1, 0, 1, fill);
  if (getWidth(l, pos) == 0 && !hasContent(l, pos)) setCellCp(l, pos, 0, 1, fill);
}

void replaceCells(Line& l, int start, int end, const Cell& fill, bool respectProtect) {
  const int len = lineLen(l);
  if (respectProtect) {
    if (start && getWidth(l, start - 1) == 2 && !isProtected(l, start - 1)) setCellCp(l, start - 1, 0, 1, fill);
    if (end < len && getWidth(l, end - 1) == 2 && !isProtected(l, end)) setCellCp(l, end, 0, 1, fill);
    while (start < end && start < len) {
      if (!isProtected(l, start)) setCell(l, start, fill);
      start++;
    }
    return;
  }
  // handle fullwidth at start: reset cell one to the left if start is second cell of a wide char
  if (start && getWidth(l, start - 1) == 2) setCellCp(l, start - 1, 0, 1, fill);
  // handle fullwidth at last cell + 1: reset to empty cell if it is second part of a wide char
  if (end < len && getWidth(l, end - 1) == 2) setCellCp(l, end, 0, 1, fill);
  if (start < 0) start = 0;
  while (start < end && start < len) l.cells[start++] = fill;
}

void fillLine(Line& l, const Cell& fill, bool respectProtect) {
  if (respectProtect) {
    for (auto& c : l.cells) if (!(c.flags & Protected)) c = fill;
    return;
  }
  std::fill(l.cells.begin(), l.cells.end(), fill);
}

void resizeLine(Line& l, int cols, const Cell& fill) {
  if (cols == lineLen(l)) return;
  l.cells.resize((size_t)cols, fill);       // grows with fill, or truncates
}

int trimmedLength(const Line& l) {
  for (int i = lineLen(l) - 1; i >= 0; --i) {
    const Cell& c = l.cells[i];
    if (c.ch != 0 || c.extra != 0) return i + c.width;
  }
  return 0;
}

void copyCellsFrom(Line& dst, const Line& src, int srcCol, int destCol, int length, bool reverse) {
  const int sl = lineLen(src), dl = lineLen(dst);
  auto one = [&](int cell) {
    const int si = srcCol + cell, di = destCol + cell;
    if (di < 0 || di >= dl) return;
    if (si < 0 || si >= sl) { Cell z{}; dst.cells[di] = z; return; }      // typed array read of undefined -> 0
    dst.cells[di] = src.cells[si];
  };
  if (reverse) { for (int cell = length - 1; cell >= 0; cell--) one(cell); }
  else { for (int cell = 0; cell < length; cell++) one(cell); }
}

void makeBlank(Line& l, int cols, const Cell& fill, bool wrapped) {
  l.cells.assign((size_t)cols, fill);
  l.wrapped = wrapped;
}

// ------------------------------------------------------------------------------------------------ Buffer

void Buffer::init(bool hasSb, int sb, int c, int r) {
  hasScrollback = hasSb;
  scrollback = sb;
  cols = c; rows = r;
  lines.clear();
  maxLength = correctBufferLength(r);
  startIndex = 0;
  ybase = y = x = 0;
  scrollTop = 0; scrollBottom = r - 1;
  savedX = savedY = 0; savedFg = savedBg = 0; savedCharset = 0;
  tabs.clear();
  setupTabStops();
}

void Buffer::fillViewportRows(const Cell& fill) {
  if (!lines.empty()) return;
  for (int i = 0; i < rows; ++i) { Line l; makeBlank(l, cols, fill, false); lines.push_back(std::move(l)); }
}

void Buffer::clear() {
  ybase = 0; y = 0; x = 0;
  lines.clear();
  maxLength = correctBufferLength(rows);
  startIndex = 0;
  scrollTop = 0;
  scrollBottom = rows - 1;
  setupTabStops();
}

void Buffer::push(Line&& l) {
  if (lines.size() >= maxLength && !lines.empty()) { lines.pop_front(); startIndex = (startIndex + 1) % maxLength; }
  lines.push_back(std::move(l));
}

void Buffer::trimStart(size_t n) {
  if (n > lines.size()) n = lines.size();
  startIndex += n;
  lines.erase(lines.begin(), lines.begin() + (std::ptrdiff_t)n);
}

void Buffer::setTab(int i, bool v) {
  if (i < 0) return;
  if (i >= (int)tabs.size()) { if (!v) return; tabs.resize((size_t)i + 1, 0); }
  tabs[(size_t)i] = v ? 1 : 0;
}

void Buffer::setupTabStops(int i) {
  if (i >= 0) {
    if (!tabAt(i)) i = prevStop(i);
  } else {
    tabs.assign(tabs.size(), 0);
    i = 0;
  }
  for (; i < cols; i += 8) setTab(i, true);
}

int Buffer::prevStop(int px) const {
  while (!tabAt(--px) && px > 0) {}
  return px >= cols ? cols - 1 : px < 0 ? 0 : px;
}

int Buffer::nextStop(int nx) const {
  while (!tabAt(++nx) && nx < cols) {}
  return nx >= cols ? cols - 1 : nx < 0 ? 0 : nx;
}

// ---- BufferReflow.ts

namespace {

// getWrappedLineTrimmedLength over a contiguous run of lines addressed by an accessor
template <class Get>
int wrappedTrimmedLength(Get&& get, int count, int i, int cols) {
  if (i == count - 1) return trimmedLength(get(i));
  const Line& li = get(i);
  const bool endsInNull = !hasContent(li, cols - 1) && getWidth(li, cols - 1) == 1;
  const bool followingLineStartsWithWide = getWidth(get(i + 1), 0) == 2;
  if (endsInNull && followingLineStartsWithWide) return cols - 1;
  return cols;
}

void reflowLarger(Buffer& b, int newCols, int newRows, const Cell& nullCell) {
  auto& lines = b.lines;
  const int oldCols = b.cols;
  const int absY = b.ybase + b.y;
  std::vector<int> toRemove;                 // pairs: index, count
  for (int y = 0; y < (int)lines.size() - 1; y++) {
    int i = y + 1;
    if (!lines[(size_t)i].wrapped) continue;
    // Check how many lines it's wrapped for
    while (i < (int)lines.size() && lines[(size_t)i].wrapped) ++i;
    const int n = i - y;                     // wrapped lines y .. i-1
    // If these lines contain the cursor don't touch them, the program will handle fixing up wrapped lines
    // with the cursor (reflowCursorLine is off)
    if (absY >= y && absY < i) { y += n - 1; continue; }
    auto get = [&](int k) -> Line& { return lines[(size_t)(y + k)]; };
    // Copy buffer data to new locations
    int destLineIndex = 0;
    int destCol = wrappedTrimmedLength(get, n, destLineIndex, oldCols);
    int srcLineIndex = 1;
    int srcCol = 0;
    while (srcLineIndex < n && destLineIndex < n) {
      const int srcTrimmedLineLength = wrappedTrimmedLength(get, n, srcLineIndex, oldCols);
      const int srcRemainingCells = srcTrimmedLineLength - srcCol;
      const int destRemainingCells = newCols - destCol;
      const int cellsToCopy = std::min(srcRemainingCells, destRemainingCells);
      copyCellsFrom(get(destLineIndex), get(srcLineIndex), srcCol, destCol, cellsToCopy, false);
      destCol += cellsToCopy;
      if (destCol == newCols) { destLineIndex++; destCol = 0; }
      srcCol += cellsToCopy;
      if (srcCol == srcTrimmedLineLength) { srcLineIndex++; srcCol = 0; }
      // Make sure the last cell isn't wide, if it is copy it to the current dest
      if (destCol == 0 && destLineIndex != 0 && destLineIndex < n) {
        if (getWidth(get(destLineIndex - 1), newCols - 1) == 2) {
          copyCellsFrom(get(destLineIndex), get(destLineIndex - 1), newCols - 1, destCol++, 1, false);
          // Null out the end of the last row
          setCell(get(destLineIndex - 1), newCols - 1, nullCell);
        }
      }
    }
    if (destLineIndex < n) {
      // Clear out remaining cells or fragments could remain
      replaceCells(get(destLineIndex), destCol, newCols, nullCell);
    }
    // Work backwards and remove any rows at the end that only contain null cells
    int countToRemove = 0;
    for (int k = n - 1; k > 0; k--) {
      if (k > destLineIndex || trimmedLength(get(k)) == 0) countToRemove++;
      else break;
    }
    if (countToRemove > 0) {
      toRemove.push_back(y + n - countToRemove);
      toRemove.push_back(countToRemove);
    }
    y += n - 1;
  }
  if (toRemove.empty()) return;
  // reflowLargerCreateNewLayout + reflowLargerApplyNewLayout
  int countRemoved = 0;
  {
    std::deque<Line> kept;
    size_t next = 0;
    for (int i = 0; i < (int)lines.size(); i++) {
      if (next < toRemove.size() && toRemove[next] == i) {
        const int c = toRemove[next + 1];
        i += c - 1;
        countRemoved += c;
        next += 2;
      } else {
        kept.push_back(std::move(lines[(size_t)i]));
      }
    }
    lines.swap(kept);
  }
  // _reflowLargerAdjustViewport
  int viewportAdjustments = countRemoved;
  while (viewportAdjustments-- > 0) {
    if (b.ybase == 0) {
      if (b.y > 0) b.y--;
      if ((int)lines.size() < newRows) {
        // Add an extra row at the bottom of the viewport
        Line l; makeBlank(l, newCols, nullCell, false);
        b.push(std::move(l));
      }
    } else {
      b.ybase--;
    }
  }
  b.savedY = std::max(b.savedY - countRemoved, 0);
}

struct Insert { int start; std::vector<Line> newLines; };

void reflowSmaller(Buffer& b, int newCols, int newRows, const Cell& nullCell, int bufferServiceCols) {
  auto& lines = b.lines;
  const int oldCols = b.cols;
  // Gather all BufferLines that need to be inserted into the Buffer here so that they can be batched up and
  // only committed once
  std::vector<Insert> toInsert;
  int countToInsert = 0;
  // CircularList.pop() only shortens the list: the popped line stays readable by index, and the loop below
  // (which walks down from the ORIGINAL last index) still visits it. `length` is the list's logical length;
  // the physical deque is cut to it at the end.
  int length = (int)lines.size();
  // Go backwards as many lines may be trimmed and this will avoid considering them
  for (int y = (int)lines.size() - 1; y >= 0; y--) {
    // Check whether this line is a problem
    if (!lines[(size_t)y].wrapped && trimmedLength(lines[(size_t)y]) <= newCols) continue;
    // Gather wrapped lines and adjust y to be the starting line
    const int last = y;
    while (lines[(size_t)y].wrapped && y > 0) --y;
    const int n = last - y + 1;
    // If these lines contain the cursor don't touch them (reflowCursorLine is off)
    const int absoluteY = b.ybase + b.y;
    if (absoluteY >= y && absoluteY < y + n) continue;

    std::vector<Line> newLines;
    auto get = [&](int k) -> Line& { return k < n ? lines[(size_t)(y + k)] : newLines[(size_t)(k - n)]; };

    const int lastLineLength = trimmedLength(lines[(size_t)last]);
    // reflowSmallerGetNewLineLengths
    std::vector<int> destLineLengths;
    {
      int cellsNeeded = 0;
      for (int k = 0; k < n; ++k) cellsNeeded += wrappedTrimmedLength(get, n, k, oldCols);
      int srcCol = 0, srcLine = 0, cellsAvailable = 0;
      while (cellsAvailable < cellsNeeded) {
        if (cellsNeeded - cellsAvailable < newCols) {
          // Add the final line and exit the loop
          destLineLengths.push_back(cellsNeeded - cellsAvailable);
          break;
        }
        srcCol += newCols;
        const int oldTrimmedLength = srcLine < n ? wrappedTrimmedLength(get, n, srcLine, oldCols) : 0;
        if (srcCol > oldTrimmedLength) { srcCol -= oldTrimmedLength; srcLine++; }
        const bool endsWithWide = srcLine < n && getWidth(get(srcLine), srcCol - 1) == 2;
        if (endsWithWide) srcCol--;
        const int lineLength = endsWithWide ? newCols - 1 : newCols;
        destLineLengths.push_back(lineLength);
        cellsAvailable += lineLength;
      }
    }
    const int destCount = (int)destLineLengths.size();
    const int linesToAdd = destCount - n;
    int trimmedLines;
    if (b.ybase == 0 && b.y != length - 1) {
      // If the top section of the buffer is not yet filled
      trimmedLines = std::max(0, b.y - (int)b.maxLength + linesToAdd);
    } else {
      trimmedLines = std::max(0, length - (int)b.maxLength + linesToAdd);
    }
    // Add the new lines
    for (int k = 0; k < linesToAdd; k++) { Line l; makeBlank(l, bufferServiceCols, nullCell, true); newLines.push_back(std::move(l)); }
    const int insertStart = y + n + countToInsert;
    if (!newLines.empty()) countToInsert += (int)newLines.size();
    const int total = n + (int)newLines.size();          // wrappedLines.length after the push

    // Copy buffer data to new locations, this needs to happen backwards to do in-place
    int destLineIndex = destCount - 1;
    auto destLen = [&](int k) { return (k >= 0 && k < destCount) ? destLineLengths[(size_t)k] : -1; };   // -1 = undefined
    int destCol = destLen(destLineIndex);
    if (destCol == 0) { destLineIndex--; destCol = destLen(destLineIndex); }
    int srcLineIndex = total - linesToAdd - 1;
    int srcCol = lastLineLength;
    while (srcLineIndex >= 0) {
      if (destLineIndex < 0 || destLineIndex >= total || destCol < 0) break;       // wrappedLines[dest] undefined / NaN
      if (srcLineIndex >= total) break;                                            // xterm would throw here
      const int cellsToCopy = std::min(srcCol, destCol);
      copyCellsFrom(get(destLineIndex), get(srcLineIndex), srcCol - cellsToCopy, destCol - cellsToCopy, cellsToCopy, true);
      destCol -= cellsToCopy;
      if (destCol == 0) { destLineIndex--; destCol = destLen(destLineIndex); }
      srcCol -= cellsToCopy;
      if (srcCol == 0) {
        srcLineIndex--;
        const int wrappedLinesIndex = std::max(srcLineIndex, 0);
        srcCol = wrappedTrimmedLength(get, total, wrappedLinesIndex, oldCols);
      }
    }
    // Null out the end of the line ends if a wide character wrapped to the following line
    for (int k = 0; k < total; k++) {
      const int dl = destLen(k);
      if (dl >= 0 && dl < newCols) setCell(get(k), dl, nullCell);
    }
    // Adjust viewport as needed
    int viewportAdjustments = linesToAdd - trimmedLines;
    while (viewportAdjustments-- > 0) {
      if (b.ybase == 0) {
        if (b.y < newRows - 1) {
          b.y++;
          length--;
        } else {
          b.ybase++;
        }
      } else {
        // Ensure ybase does not exceed its maximum value
        if (b.ybase < std::min((int)b.maxLength, length + countToInsert) - newRows) b.ybase++;
      }
    }
    b.savedY = std::min(b.savedY + linesToAdd, b.ybase + newRows - 1);
    if (!newLines.empty()) toInsert.push_back(Insert{insertStart, std::move(newLines)});
  }

  // Rearrange lines in the buffer if there are any insertions, this is done at the end rather than earlier
  // so that it's a single O(n) pass through the buffer, instead of O(n^2) from many splices.
  if (length < 0) length = 0;
  if ((int)lines.size() > length) lines.erase(lines.begin() + length, lines.end());
  if (toInsert.empty()) return;
  std::vector<Line> original;
  original.reserve(lines.size());
  for (auto& l : lines) original.push_back(std::move(l));
  const int originalLinesLength = (int)original.size();
  int originalLineIndex = originalLinesLength - 1;
  size_t nextToInsertIndex = 0;
  const int newLength = std::min((int)b.maxLength, originalLinesLength + countToInsert);
  std::vector<Line> result((size_t)std::max(newLength, 0));
  int countInsertedSoFar = 0;
  for (int i = std::min((int)b.maxLength - 1, originalLinesLength + countToInsert - 1); i >= 0; i--) {
    if (nextToInsertIndex < toInsert.size() && toInsert[nextToInsertIndex].start > originalLineIndex + countInsertedSoFar) {
      // Insert extra lines here, adjusting i as needed
      auto& nl = toInsert[nextToInsertIndex].newLines;
      for (int nextI = (int)nl.size() - 1; nextI >= 0; nextI--) {
        if (i >= 0) result[(size_t)i] = std::move(nl[(size_t)nextI]);
        else if ((long long)b.startIndex + i >= 0 && newLength + i >= 0) {
          // xterm quirk, kept: CircularList.set() with a negative index lands on the slot before
          // _startIndex, which in a full list is the list's END. So when the top of a full scrollback cuts
          // through a group of freshly wrapped rows, the cut-off rows overwrite the buffer's last lines.
          result[(size_t)(newLength + i)] = std::move(nl[(size_t)nextI]);
        }
        i--;
      }
      i++;
      countInsertedSoFar += (int)nl.size();
      nextToInsertIndex++;
    } else {
      if (originalLineIndex >= 0) result[(size_t)i] = std::move(original[(size_t)originalLineIndex]);
      originalLineIndex--;
    }
  }
  lines.clear();
  for (auto& l : result) lines.push_back(std::move(l));
}

}  // namespace

void Buffer::resize(int newCols, int newRows, const Cell& nullCell, int bufferServiceCols) {
  // Increase max length if needed before adjustments to allow space to fill as required
  const size_t newMaxLength = correctBufferLength(newRows);
  if (newMaxLength > maxLength) setMaxLength(newMaxLength);

  // The following adjustments should only happen if the buffer has been initialized/filled
  if (!lines.empty()) {
    // Deal with columns increasing (reducing needs to happen after reflow)
    if (cols < newCols) {
      for (auto& l : lines) resizeLine(l, newCols, nullCell);
    }
    // Resize rows in both directions as needed
    if (rows < newRows) {
      for (int yy = rows; yy < newRows; yy++) {
        if ((int)lines.size() < newRows + ybase) {
          // windowsPty is set: just add the new missing rows (ConPTY repaints; it does not want the
          // scrollback pulled down)
          Line l; makeBlank(l, newCols, nullCell, false);
          push(std::move(l));
        }
      }
    } else {
      for (int yy = rows; yy > newRows; yy--) {
        if ((int)lines.size() > newRows + ybase) {
          if ((int)lines.size() > ybase + y + 1) {
            // The line is a blank line below the cursor, remove it
            lines.pop_back();
          } else {
            // The line is the cursor, scroll down
            ybase++;
          }
        }
      }
    }
    // Reduce max length if needed after adjustments, this is done after as it would otherwise cut data from
    // the bottom of the buffer.
    if (newMaxLength < maxLength) {
      // Trim from the top of the buffer and adjust ybase.
      const long long amountToTrim = (long long)lines.size() - (long long)newMaxLength;
      if (amountToTrim > 0) {
        trimStart((size_t)amountToTrim);
        ybase = std::max(ybase - (int)amountToTrim, 0);
        savedY = std::max(savedY - (int)amountToTrim, 0);
      }
      setMaxLength(newMaxLength);
    }
    // Make sure that the cursor stays on screen
    x = std::min(x, newCols - 1);
    y = std::min(y, newRows - 1);
    savedX = std::min(savedX, newCols - 1);
    scrollTop = 0;
  }
  scrollBottom = newRows - 1;

  // Reflow: on for the buffer with scrollback (windowsPty conpty, build >= 21376), never for the alt buffer
  if (hasScrollback) {
    if (cols != newCols) {
      if (newCols > cols) reflowLarger(*this, newCols, newRows, nullCell);
      else reflowSmaller(*this, newCols, newRows, nullCell, bufferServiceCols);
    }
    // Trim the end of the line off if cols shrunk
    if (cols > newCols) {
      for (auto& l : lines) resizeLine(l, newCols, nullCell);
    }
  }
  cols = newCols;
  rows = newRows;
}

}}}  // namespace deck::vt::detail
