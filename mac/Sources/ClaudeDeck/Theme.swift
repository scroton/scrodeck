// mac/Sources/ClaudeDeck/Theme.swift — the deck's look as numbers, in one place: the chrome's colours and sizes
// from renderer/style.css (:root and the .tile rules), the terminal's theme from renderer/js/20-terminals.js THEME.
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain). Windows counterpart: the constants at the top of
// windows/src/sidebar.cpp and `struct Theme` in windows/src/termview.hpp. Change a number HERE only when the CSS changed.
import AppKit

enum Theme {
  // style.css :root
  static let bg = rgb(0x0c0c0c), sidebar = rgb(0x0f0f0f), tile = rgb(0x171717), tileHover = rgb(0x1b1b1b), tileActive = rgb(0x222222)
  static let tileActiveBorder = rgb(0xbdbdbd), tileBorder = rgb(0x2c2c2c), box = rgb(0x242424)
  static let fg = rgb(0xececec), fgDim = rgb(0x9a9a9a), fgFaint = rgb(0x6a6a6a)
  static let green = rgb(0x3ecf5a), track = rgb(0x3c3c3c), btn = rgb(0x262626), btnBorder = rgb(0x383838), danger = rgb(0xc94a4a)
  static let cloud = rgb(0x8fb8e8), attention = rgb(0xf0b429)
  static let menuBg = rgb(0x1c1c1c), menuBorder = rgb(0x3a3a3a)
  // the asks (style.css: :root --ask / --ask-ink / --ask-dim, .asks-tile)
  static let ask = rgb(0x6aa9ff), askInk = rgb(0x0b1220), askDim = rgb(0x4a6c99)
  static let asksTileBg = rgb(0x1a1710), asksTileBorder = rgb(0x3a3020), asksTileActive = rgb(0x262013)
  static let sidebarWidth: CGFloat = 340, sidebarMinWidth: CGFloat = 220, sidebarMaxFraction: CGFloat = 0.70
  static let tilePadX: CGFloat = 4, tilePadY: CGFloat = 6, tileGap: CGFloat = 1
  static let abbrFont = NSFont.systemFont(ofSize: 13, weight: .semibold)
  static let titleFont = NSFont.systemFont(ofSize: 14)
  static let ageFont = NSFont.monospacedDigitSystemFont(ofSize: 12, weight: .regular)
  static let subFont = NSFont.systemFont(ofSize: 11.5)
  static let effortFont = NSFont.systemFont(ofSize: 10)
  static let menuFont = NSFont.systemFont(ofSize: 12.5)
  // the pane's name band: one line of the pane-name size (13 px) * 1.2 + 6, then 4 right / 8 bottom / 8 left (style.css .term)
  static let paneNameSize: CGFloat = 13
  static var paneNameBand: CGFloat { (paneNameSize * 1.2 + 6).rounded() }
  static let panePadRight: CGFloat = 4, panePadBottom: CGFloat = 8, panePadLeft: CGFloat = 8

  // 20-terminals.js THEME (0xRRGGBB)
  static let termBackground: UInt32 = 0x0c0c0c, termForeground: UInt32 = 0xe6e6e6, termCursor: UInt32 = 0xe6e6e6, termCursorAccent: UInt32 = 0x0c0c0c, termSelection: UInt32 = 0x3a3d41
  static let ansi: [UInt32] = [0x0c0c0c, 0xc50f1f, 0x13a10e, 0xc19c00, 0x0037da, 0x881798, 0x3a96dd, 0xcccccc,
                               0x767676, 0xe74856, 0x16c60c, 0xf9f1a5, 0x3b78ff, 0xb4009e, 0x61d6d6, 0xf2f2f2]
  static let terminalFontSize: CGFloat = 14, terminalLineHeight: CGFloat = 1.1, scrollback: Int32 = 10000

  /// xterm's 256-colour palette (windows/src/termview.hpp Theme::palette)
  static func palette(_ i: Int) -> UInt32 {
    if i < 16 { return ansi[i] }
    if i < 232 { let n = i - 16; let v: [UInt32] = [0, 95, 135, 175, 215, 255]; return (v[n / 36] << 16) | (v[(n / 6) % 6] << 8) | v[n % 6] }
    let g = UInt32(8 + (i - 232) * 10); return (g << 16) | (g << 8) | g
  }
  static func rgb(_ v: UInt32, alpha: CGFloat = 1) -> NSColor {
    NSColor(srgbRed: CGFloat((v >> 16) & 0xFF) / 255, green: CGFloat((v >> 8) & 0xFF) / 255, blue: CGFloat(v & 0xFF) / 255, alpha: alpha)
  }
}
