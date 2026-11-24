// mac/Sources/ClaudeDeck/TerminalRenderer.swift — the terminals' picture: Metal, one glyph atlas, instanced quads,
// ONE draw call per frame for every pane a window shows. docs/NATIVE-PORT.md §7's rules, each where it is kept:
//   1 render only placed terminals   `panes` holds only what the window's layout shows; an unplaced tile has no pane here
//   2 damage tracking                a row's instances are rewritten only when TerminalPane says the row changed
//                                    (vt::Terminal::takeDamage); a frame with no damage re-presents the same buffer
//   3 persistent atlas, instanced    GlyphAtlas rasterises a glyph ONCE with CoreText, when a damaged row is copied
//     quads, one draw call           (update(), never inside draw(in:)); draw(in:) is one drawPrimitives over one buffer
//   4 MTLResourceStorageModeShared   the instance buffer and the atlas texture: written in place on unified memory
// UNBUILT, UNTESTED (written on Windows; no Swift toolchain, no Metal). Every API whose exact shape was written from
// memory is marked VERIFY.
// Windows counterpart: windows/src/termview.hpp (drawing half) + the scene / layout cache in windows/src/gfx.hpp.
// JS mirrored: xterm.js's renderer as renderer/js/20-terminals.js configures it (theme, lineHeight 1.1, cursorBlink).
import AppKit
import CoreText
import Metal
import MetalKit
import DeckCoreBridge

// ---------------------------------------------------------------------------------------------- the atlas
/// Cell-sized glyph bitmaps in one texture, for every pane of every window (one device, one atlas: the WebGL
/// context cap of the Electron deck does not exist here). A slot is one cell wide, or two for a wide character.
final class GlyphAtlas {
  struct Slot { var x: Float; var y: Float; var color: Bool }       // top-left texel; `color` = an emoji bitmap (use its own RGB)
  struct Key: Hashable { var text: String; var bold: Bool; var italic: Bool; var wide: Bool }

  let device: MTLDevice
  private(set) var texture: MTLTexture!
  private(set) var cellW = 8, cellH = 17                            // device pixels
  private(set) var baseline: CGFloat = 13                           // from the cell's top, device pixels
  private(set) var generation = 0                                   // bumped when every slot was dropped (font / scale change, atlas full)
  private var slots: [Key: Slot] = [:]
  private var fonts: [CTFont] = []                                  // regular, bold, italic, bold italic
  private var nextX = 0, nextY = 0
  private let side = 2048
  private var scratch: CGContext?
  private(set) var fontName = "", fontSize: CGFloat = 14, scale: CGFloat = 2

  init(device: MTLDevice) { self.device = device }

  /// The first family of the deck's stack this Mac has (lib/platform.js defaultFontStack: "Cascadia Mono",
  /// "SF Mono", Menlo, Monaco), the chosen one first. SF Mono is not a public family name on every macOS, so the
  /// system's monospaced font stands in for it.
  static func resolveFont(wanted: String, size: CGFloat) -> CTFont {
    for name in [wanted, "Cascadia Mono", "SF Mono", "Menlo", "Monaco"] where !name.isEmpty {
      if let f = NSFont(name: name, size: size) { return f as CTFont }
    }
    return NSFont.monospacedSystemFont(ofSize: size, weight: .regular) as CTFont
  }

  /// (Re)build for a font, size (points) and backing scale. Drops every slot: callers repaint all rows (generation).
  func configure(fontFamily: String, size: CGFloat, scale: CGFloat) {
    if fontFamily == fontName && size == fontSize && scale == self.scale && texture != nil { return }
    fontName = fontFamily; fontSize = size; self.scale = scale
    let px = size * scale
    let regular = GlyphAtlas.resolveFont(wanted: fontFamily, size: px)
    func variant(_ traits: CTFontSymbolicTraits) -> CTFont { CTFontCreateCopyWithSymbolicTraits(regular, px, nil, traits, traits) ?? regular }
    fonts = [regular, variant(.traitBold), variant(.traitItalic), variant([.traitBold, .traitItalic])]
    // xterm measures a character box and multiplies its height by lineHeight (1.1), floored; the width is the
    // advance of one monospace character. windows/src/termview.hpp measureCells does the same with DirectWrite.
    var glyph = CGGlyph(0); var ch: UniChar = 87 /* W */
    CTFontGetGlyphsForCharacters(regular, &ch, &glyph, 1)
    var advance = CGSize.zero
    CTFontGetAdvancesForGlyphs(regular, .horizontal, &glyph, &advance, 1)
    let ascent = CTFontGetAscent(regular), descent = CTFontGetDescent(regular), leading = CTFontGetLeading(regular)
    let charH = (ascent + descent + leading).rounded(.up)
    cellW = max(1, Int(advance.width.rounded()))
    cellH = max(1, Int((charH * Theme.terminalLineHeight).rounded(.down)))
    baseline = ((CGFloat(cellH) - charH) / 2 + ascent).rounded()
    let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: side, height: side, mipmapped: false)
    d.usage = .shaderRead
    d.storageMode = .shared                                         // §7 rule 4 (Apple Silicon; an Intel Mac's discrete GPU wants .managed — VERIFY on one)
    texture = device.makeTexture(descriptor: d)
    scratch = CGContext(data: nil, width: cellW * 2, height: cellH, bitsPerComponent: 8, bytesPerRow: cellW * 2 * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)   // BGRA, premultiplied
    reset()
  }
  private func reset() {
    slots.removeAll(keepingCapacity: true)
    nextX = cellW * 2; nextY = 0                                    // slot (0,0) stays empty: "no glyph"
    generation += 1
    // clear the first slot so an empty cell samples transparent
    let zero = [UInt8](repeating: 0, count: cellW * 2 * cellH * 4)
    texture.replace(region: MTLRegionMake2D(0, 0, cellW * 2, cellH), mipmapLevel: 0, withBytes: zero, bytesPerRow: cellW * 2 * 4)
  }

  /// The slot of a cell's text in a style, rasterised now if it is new. NEVER called from draw(in:).
  func slot(_ text: String, bold: Bool, italic: Bool, wide: Bool) -> Slot {
    let key = Key(text: text, bold: bold, italic: italic, wide: wide)
    if let s = slots[key] { return s }
    let w = wide ? cellW * 2 : cellW
    if nextX + w > side { nextX = 0; nextY += cellH }
    if nextY + cellH > side { reset() }                             // full: start over; every pane repaints (generation)
    let s = rasterise(text, font: fonts[(bold ? 1 : 0) + (italic ? 2 : 0)], width: w, atX: nextX, y: nextY)
    nextX += w
    slots[key] = s
    return s
  }

  private func rasterise(_ text: String, font: CTFont, width: Int, atX: Int, y: Int) -> Slot {
    guard let ctx = scratch, let data = ctx.data else { return Slot(x: 0, y: 0, color: false) }
    ctx.clear(CGRect(x: 0, y: 0, width: cellW * 2, height: cellH))
    // CTLine does the font fallback (CJK, symbols, Apple Color Emoji) that a single CTFont's glyph table cannot
    let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.white]
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attrs))
    ctx.textPosition = CGPoint(x: 0, y: CGFloat(cellH) - baseline)  // CoreGraphics' origin is the bottom-left
    CTLineDraw(line, ctx)
    // white text leaves B = G = R = A; anything else is a colour bitmap (an emoji)
    let p = data.bindMemory(to: UInt8.self, capacity: cellW * 2 * cellH * 4)
    var color = false
    let rowBytes = cellW * 2 * 4
    scan: for row in 0..<cellH { for col in 0..<width { let i = row * rowBytes + col * 4; if p[i] != p[i + 3] || p[i + 1] != p[i + 3] || p[i + 2] != p[i + 3] { color = true; break scan } } }
    // the context's rows run top to bottom in memory (row 0 = the TOP of the image) — VERIFY once on screen: a glyph
    // drawn upside down means this copy must flip
    texture.replace(region: MTLRegionMake2D(atX, y, width, cellH), mipmapLevel: 0, withBytes: p, bytesPerRow: rowBytes)
    return Slot(x: Float(atX), y: Float(y), color: color)
  }
}

// ---------------------------------------------------------------------------------------------- the renderer
/// What one quad is. 40 bytes, matched field for field by `Instance` in the shader source below (packed types).
struct CellInstance {
  var originX: Float = 0, originY: Float = 0      // device pixels, top-left of the drawable
  var sizeW: Float = 0, sizeH: Float = 0
  var uvX: Float = 0, uvY: Float = 0              // the atlas slot's top-left texel
  var fg: UInt32 = 0, bg: UInt32 = 0              // 0x00RRGGBB
  var flags: UInt32 = 0
  var pad: UInt32 = 0
}
private let kFlagColorGlyph: UInt32 = 1, kFlagUnderline: UInt32 = 2, kFlagStrike: UInt32 = 4, kFlagBar: UInt32 = 8, kFlagUnderCursor: UInt32 = 16, kFlagOutline: UInt32 = 32, kFlagDouble: UInt32 = 64

/// One pane's place in a window: which terminal, where its grid starts (device pixels), and its slice of the buffer.
final class PanePlacement {
  let pane: TerminalPane
  var gridOrigin = CGPoint.zero                   // device pixels
  var focused = false
  fileprivate var base = 0                        // first instance of its cells
  fileprivate var cursorIndex = 0                 // its cursor's instance (after every pane's cells)
  fileprivate var atlasGeneration = -1
  init(pane: TerminalPane) { self.pane = pane }
}

final class TerminalRenderer: NSObject, MTKViewDelegate {
  let device: MTLDevice
  let atlas: GlyphAtlas
  private let queue: MTLCommandQueue
  private var pipeline: MTLRenderPipelineState?
  private var sampler: MTLSamplerState?
  private var instances: MTLBuffer?
  private var instanceCount = 0
  private(set) var placements: [PanePlacement] = []
  private var rowScratch: [DBCell] = []

  static let shared: MTLDevice? = MTLCreateSystemDefaultDevice()
  /// one atlas for the process (every window, every pane)
  static let sharedAtlas: GlyphAtlas? = shared.map(GlyphAtlas.init)

  init?(view: MTKView) {
    guard let dev = TerminalRenderer.shared, let atlas = TerminalRenderer.sharedAtlas, let q = dev.makeCommandQueue() else { return nil }
    device = dev; self.atlas = atlas; queue = q
    super.init()
    assert(MemoryLayout<CellInstance>.stride == 40, "CellInstance must match the shader's packed Instance (40 bytes)")
    view.device = dev
    view.colorPixelFormat = .bgra8Unorm
    view.clearColor = MTLClearColor(red: 12 / 255, green: 12 / 255, blue: 12 / 255, alpha: 1)      // #0c0c0c
    view.isPaused = true; view.enableSetNeedsDisplay = true          // a frame only when something changed
    view.delegate = self
    do {
      let lib = try dev.makeLibrary(source: TerminalRenderer.shaderSource, options: nil)           // compiled at launch: `swift build` has no .metal step
      let d = MTLRenderPipelineDescriptor()
      d.vertexFunction = lib.makeFunction(name: "cell_vertex"); d.fragmentFunction = lib.makeFunction(name: "cell_fragment")
      d.colorAttachments[0].pixelFormat = view.colorPixelFormat
      pipeline = try dev.makeRenderPipelineState(descriptor: d)
    } catch { NSLog("%@: the terminal shader did not build: %@", Edition.productName, String(describing: error)); return nil }
    let s = MTLSamplerDescriptor(); s.minFilter = .nearest; s.magFilter = .nearest                // glyphs are placed on whole pixels
    sampler = dev.makeSamplerState(descriptor: s)
  }

  var cellSize: CGSize { CGSize(width: atlas.cellW, height: atlas.cellH) }                        // device pixels

  /// The window's layout changed: these panes, at these places. Rebuilds the buffer; every row is copied again.
  func setPlacements(_ p: [PanePlacement]) {
    placements = p
    var n = 0
    for pl in p { pl.base = n; n += pl.pane.cols * pl.pane.rows; pl.pane.markAllDirty() }
    for pl in p { pl.cursorIndex = n; n += 1 }
    instanceCount = n
    let bytes = max(1, n) * MemoryLayout<CellInstance>.stride
    if instances == nil || instances!.length < bytes { instances = device.makeBuffer(length: bytes * 2, options: .storageModeShared) }
  }

  /// Copy what changed into the instance buffer (this is where CoreText may run, for glyphs seen for the first
  /// time). Returns true when a frame is needed. Called after a feed, a scroll, a selection change, a blink.
  @discardableResult func update() -> Bool {
    guard let buf = instances else { return false }
    let out = buf.contents().bindMemory(to: CellInstance.self, capacity: max(1, instanceCount))
    var changed = false
    for pl in placements {
      let pane = pl.pane
      if pl.atlasGeneration != atlas.generation { pl.atlasGeneration = atlas.generation; pane.markAllDirty() }
      if let d = pane.takeDirty() {
        changed = true
        if rowScratch.count < pane.cols { rowScratch = [DBCell](repeating: DBCell(), count: pane.cols) }
        for row in max(0, d.lowerBound)...min(pane.rows - 1, d.upperBound) { writeRow(row, of: pl, to: out) }
      }
      if writeCursor(of: pl, to: out) { changed = true }
    }
    return changed
  }

  private func color(_ c: UInt32, default d: UInt32) -> UInt32 {
    if c == DB_COLOR_DEFAULT { return d }
    if c & 0xFF000000 == DB_COLOR_PALETTE { return Theme.palette(Int(c & 0xFF)) }
    return c & 0xFFFFFF
  }

  private func writeRow(_ row: Int, of pl: PanePlacement, to out: UnsafeMutablePointer<CellInstance>) {
    let pane = pl.pane, cols = pane.cols
    let n = rowScratch.withUnsafeMutableBufferPointer { pane.readRow(row, into: $0.baseAddress!) }
    let cw = Float(atlas.cellW), ch = Float(atlas.cellH)
    let y = Float(pl.gridOrigin.y) + Float(row) * ch
    let line = pane.topLine + row
    for col in 0..<cols {
      var inst = CellInstance()
      inst.originX = Float(pl.gridOrigin.x) + Float(col) * cw; inst.originY = y
      inst.sizeW = cw; inst.sizeH = ch
      inst.fg = Theme.termForeground; inst.bg = Theme.termBackground
      if col < n {
        let c = rowScratch[col]
        let flags = Int32(c.flags)
        var fg = color(c.fg, default: Theme.termForeground), bg = color(c.bg, default: Theme.termBackground)
        // xterm draws bold text in the bright colour when the colour is one of the first eight (drawBoldTextInBrightColors)
        if flags & DB_BOLD != 0, c.fg & 0xFF000000 == DB_COLOR_PALETTE, (c.fg & 0xFF) < 8 { fg = Theme.palette(Int(c.fg & 0xFF) + 8) }
        if flags & DB_INVERSE != 0 { swap(&fg, &bg) }
        if flags & DB_DIM != 0 { fg = TerminalRenderer.blend(fg, bg, 0.5) }
        if let s = pane.selection, s.contains(x: col, line: line) { bg = Theme.termSelection }
        inst.fg = fg; inst.bg = bg
        if c.width == 0 {
          inst.sizeW = 0                                             // the right half of a wide character: its left half's quad covers it
        } else {
          if c.width == 2 { inst.sizeW = cw * 2 }
          if c.ch != 0 && c.ch != 32 && flags & DB_INVISIBLE == 0 {
            let scalars: [UInt32] = c.cluster != 0 ? pane.cluster(row: row, col: col) : [c.ch]
            var text = ""; for u in scalars { if let s = Unicode.Scalar(u) { text.unicodeScalars.append(s) } }
            let slot = atlas.slot(text, bold: flags & DB_BOLD != 0, italic: flags & DB_ITALIC != 0, wide: c.width == 2)
            inst.uvX = slot.x; inst.uvY = slot.y
            if slot.color { inst.flags |= kFlagColorGlyph }
          }
          if flags & DB_UNDERLINE != 0 {
            inst.flags |= kFlagUnderline
            if (flags & DB_UNDERLINE_STYLE_MASK) >> DB_UNDERLINE_STYLE_SHIFT == 2 { inst.flags |= kFlagDouble }
            // curly / dotted / dashed are drawn as a plain line in this draft (mac/PARITY.md)
          }
          if flags & DB_STRIKE != 0 { inst.flags |= kFlagStrike }
        }
      }
      out[pl.base + row * cols + col] = inst
    }
  }
  // (DB_* are #defines of plain literals in DeckBridge.h, so Swift sees them as Int32 / UInt32 constants.)

  /// The cursor's quad: block (inverted glyph), underline, bar; an outline when the pane is not focused; hidden
  /// while scrolled back, while DECTCEM says so, and in the blink's off phase.
  private func writeCursor(of pl: PanePlacement, to out: UnsafeMutablePointer<CellInstance>) -> Bool {
    let pane = pl.pane, st = pane.state
    var inst = CellInstance()
    let visible = st.cursor_visible != 0 && pane.scrollOffset == 0 && pane.exitLine == nil && (pane.cursorOn || !pl.focused)
    if visible {
      let cw = Float(atlas.cellW), ch = Float(atlas.cellH)
      inst.originX = Float(pl.gridOrigin.x) + Float(st.cursor_x) * cw; inst.originY = Float(pl.gridOrigin.y) + Float(st.cursor_y) * ch
      inst.sizeW = cw; inst.sizeH = ch
      inst.bg = Theme.termCursor; inst.fg = Theme.termCursorAccent
      let under = out[pl.base + Int(st.cursor_y) * pane.cols + Int(st.cursor_x)]
      if !pl.focused { inst.flags = kFlagOutline; inst.fg = Theme.termCursor; inst.bg = under.bg; inst.uvX = under.uvX; inst.uvY = under.uvY; inst.fg = under.fg }
      else if st.cursor_shape == 2 { inst.flags = kFlagUnderCursor; inst.bg = under.bg; inst.fg = under.fg; inst.uvX = under.uvX; inst.uvY = under.uvY }
      else if st.cursor_shape == 3 { inst.flags = kFlagBar; inst.bg = under.bg; inst.fg = under.fg; inst.uvX = under.uvX; inst.uvY = under.uvY }
      else { inst.uvX = under.uvX; inst.uvY = under.uvY; inst.flags = under.flags & kFlagColorGlyph }      // block: the glyph in the accent colour
      if under.sizeW > cw { inst.sizeW = under.sizeW }
    }
    let old = out[pl.cursorIndex]
    out[pl.cursorIndex] = inst
    return old.originX != inst.originX || old.originY != inst.originY || old.sizeW != inst.sizeW || old.flags != inst.flags || old.uvX != inst.uvX || old.uvY != inst.uvY
  }

  static func blend(_ a: UInt32, _ b: UInt32, _ t: Float) -> UInt32 {
    func ch(_ s: UInt32) -> UInt32 { let x = Float((a >> s) & 0xFF), y = Float((b >> s) & 0xFF); return UInt32(x + (y - x) * t) & 0xFF }
    return (ch(16) << 16) | (ch(8) << 8) | ch(0)
  }

  // ---- MTKViewDelegate
  func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
  /// One pass, one draw call. No CoreText, no allocation, no terminal read: the buffer already holds the picture.
  func draw(in view: MTKView) {
    guard let pipeline = pipeline, let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
          let cmd = queue.makeCommandBuffer(), let enc = cmd.makeRenderCommandEncoder(descriptor: pass) else { return }
    if let buf = instances, instanceCount > 0, let tex = atlas.texture {
      var uniforms = (Float(view.drawableSize.width), Float(view.drawableSize.height), Float(atlas.cellW), Float(atlas.cellH), Float(tex.width), Float(tex.height), Float(0), Float(0))
      enc.setRenderPipelineState(pipeline)
      enc.setVertexBuffer(buf, offset: 0, index: 0)
      enc.setVertexBytes(&uniforms, length: MemoryLayout.size(ofValue: uniforms), index: 1)
      enc.setFragmentBytes(&uniforms, length: MemoryLayout.size(ofValue: uniforms), index: 1)
      enc.setFragmentTexture(tex, index: 0)
      if let s = sampler { enc.setFragmentSamplerState(s, index: 0) }
      enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: instanceCount)
    }
    enc.endEncoding()
    cmd.present(drawable)
    cmd.commit()
  }

  // Metal Shading Language, built at launch (makeLibrary(source:)). A quad per instance from its instance id; the
  // fragment mixes the cell's background and foreground by the atlas's coverage, or takes an emoji's own colours.
  static let shaderSource = """
  #include <metal_stdlib>
  using namespace metal;
  struct Instance { packed_float2 origin; packed_float2 size; packed_float2 uv; uint fg; uint bg; uint flags; uint pad; };
  struct Uniforms { float2 viewport; float2 cell; float2 atlas; float2 pad; };
  struct V { float4 position [[position]]; float2 uv; float2 local; float3 fg; float3 bg; uint flags [[flat]]; float2 size; };
  static float3 rgb(uint c) { return float3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0; }
  vertex V cell_vertex(uint vid [[vertex_id]], uint iid [[instance_id]], const device Instance* instances [[buffer(0)]], constant Uniforms& u [[buffer(1)]]) {
    Instance i = instances[iid];
    float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
    float2 size = float2(i.size);
    float2 px = float2(i.origin) + corner * size;
    V o;
    o.position = float4(px.x / u.viewport.x * 2.0 - 1.0, 1.0 - px.y / u.viewport.y * 2.0, 0.0, 1.0);
    o.uv = (float2(i.uv) + corner * size) / u.atlas;
    o.local = corner * size; o.size = size;
    o.fg = rgb(i.fg); o.bg = rgb(i.bg); o.flags = i.flags;
    return o;
  }
  fragment float4 cell_fragment(V in, texture2d<float> atlas [[texture(0)]], sampler s [[sampler(0)]], constant Uniforms& u [[buffer(1)]]) {
    float4 t = atlas.sample(s, in.uv);
    float3 c = (in.flags & 1u) != 0u ? in.bg * (1.0 - t.a) + t.rgb      // a colour glyph: premultiplied, its own colours
                                    : mix(in.bg, in.fg, t.a);            // coverage
    float line = max(1.0, floor(u.cell.y / 14.0));
    if ((in.flags & 2u) != 0u && in.local.y >= u.cell.y - 2.0 * line && in.local.y < u.cell.y - line) c = in.fg;         // underline
    if ((in.flags & 64u) != 0u && in.local.y >= u.cell.y - 4.0 * line && in.local.y < u.cell.y - 3.0 * line) c = in.fg;  // the second line of a double underline
    if ((in.flags & 4u) != 0u && in.local.y >= floor(u.cell.y * 0.5) && in.local.y < floor(u.cell.y * 0.5) + line) c = in.fg;   // strike
    float3 cursor = float3(230.0, 230.0, 230.0) / 255.0;                 // THEME.cursor #e6e6e6
    if ((in.flags & 16u) != 0u && in.local.y >= u.cell.y - 2.0 * line) c = cursor;                                    // underline cursor
    if ((in.flags & 8u) != 0u && in.local.x < max(1.0, floor(u.cell.x / 8.0))) c = cursor;                             // bar cursor
    if ((in.flags & 32u) != 0u && (in.local.x < 1.0 || in.local.y < 1.0 || in.local.x >= in.size.x - 1.0 || in.local.y >= in.size.y - 1.0)) c = cursor;   // outline (no focus)
    return float4(c, 1.0);
  }
  """
}
