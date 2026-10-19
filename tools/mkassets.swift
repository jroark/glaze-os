// Glaze asset generator (macOS host): rasterizes fonts with CoreText and
// draws icons, wallpapers, sample photos and UI parts with CoreGraphics.
// Output formats are described in lib/include/glaze/gfx.h.
//
// usage: swift tools/mkassets.swift <out-dir> <font-dir>
//
// All artwork here is original, drawn in the style of early iPhone OS.

import CoreGraphics
import CoreText
import Foundation

let outDir = CommandLine.arguments[1]
let fontDir = CommandLine.arguments[2]

// MARK: - Output

func le16(_ v: Int) -> [UInt8] { [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF)] }
func le32(_ v: Int) -> [UInt8] { le16(v & 0xFFFF) + le16((v >> 16) & 0xFFFF) }

let bayer: [[Int]] = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

func save(_ name: String, _ bytes: [UInt8]) {
    let url = URL(fileURLWithPath: outDir).appendingPathComponent(name)
    try! Data(bytes).write(to: url)
}

/// Writes premultiplied RGBA as a GZI (RGB565 dithered + optional A8).
func writeGZI(_ name: String, _ w: Int, _ h: Int, _ rgba: [UInt8], alpha: Bool) {
    var out = Array("GZI1".utf8) + le16(w) + le16(h) + le16(alpha ? 1 : 0) + le16(0)
    var px = [UInt8](), al = [UInt8]()
    px.reserveCapacity(w * h * 2)
    for y in 0..<h {
        for x in 0..<w {
            let i = (y * w + x) * 4
            var r = Int(rgba[i]), g = Int(rgba[i + 1]), b = Int(rgba[i + 2])
            let a = Int(rgba[i + 3])
            if a > 0 && a < 255 {
                r = min(255, r * 255 / a); g = min(255, g * 255 / a); b = min(255, b * 255 / a)
            }
            let t = bayer[y & 3][x & 3]
            let r5 = min(31, (r + (t >> 1)) >> 3)
            let g6 = min(63, (g + (t >> 2)) >> 2)
            let b5 = min(31, (b + (t >> 1)) >> 3)
            px += le16((r5 << 11) | (g6 << 5) | b5)
            al.append(UInt8(a))
        }
    }
    out += px
    if alpha { out += al }
    save(name, out)
}

func render(_ w: Int, _ h: Int, _ draw: (CGContext) -> Void) -> [UInt8] {
    let cs = CGColorSpace(name: CGColorSpace.sRGB)!
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: cs, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.translateBy(x: 0, y: CGFloat(h))
    ctx.scaleBy(x: 1, y: -1)        // top-left origin, y down
    ctx.setShouldAntialias(true)
    draw(ctx)
    let p = ctx.data!.bindMemory(to: UInt8.self, capacity: w * h * 4)
    return Array(UnsafeBufferPointer(start: p, count: w * h * 4))
}

func image(_ name: String, _ w: Int, _ h: Int, alpha: Bool = true, _ draw: (CGContext) -> Void) {
    writeGZI(name, w, h, render(w, h, draw), alpha: alpha)
}

// MARK: - Drawing helpers

func col(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: a)
}

func gradient(_ ctx: CGContext, _ stops: [(UInt32, CGFloat, CGFloat)], from: CGPoint, to: CGPoint) {
    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
                       colors: stops.map { col($0.0, $0.1) } as CFArray,
                       locations: stops.map { $0.2 })!
    ctx.drawLinearGradient(g, start: from, end: to, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
}

func vgrad(_ ctx: CGContext, _ r: CGRect, _ top: UInt32, _ bottom: UInt32, _ a: CGFloat = 1) {
    ctx.saveGState()
    ctx.clip(to: r)
    gradient(ctx, [(top, a, 0), (bottom, a, 1)], from: CGPoint(x: 0, y: r.minY), to: CGPoint(x: 0, y: r.maxY))
    ctx.restoreGState()
}

func radial(_ ctx: CGContext, _ c: CGPoint, _ r: CGFloat, _ inner: UInt32, _ outer: UInt32, _ ai: CGFloat = 1, _ ao: CGFloat = 1) {
    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
                       colors: [col(inner, ai), col(outer, ao)] as CFArray, locations: [0, 1])!
    ctx.drawRadialGradient(g, startCenter: c, startRadius: 0, endCenter: c, endRadius: r, options: [.drawsAfterEndLocation])
}

func fill(_ ctx: CGContext, _ path: CGPath, _ c: CGColor) {
    ctx.addPath(path)
    ctx.setFillColor(c)
    ctx.fillPath()
}

func rr(_ r: CGRect, _ rad: CGFloat) -> CGPath { CGPath(roundedRect: r, cornerWidth: rad, cornerHeight: rad, transform: nil) }

struct LCG {
    var s: UInt32
    mutating func next() -> CGFloat {
        s = s &* 1_664_525 &+ 1_013_904_223
        return CGFloat(s >> 8) / CGFloat(1 << 24)
    }
}

// MARK: - Fonts

let extraChars: [UInt32] = [0x00D7, 0x00F7, 0x2212, 0x00B0, 0x2022, 0x2026, 0x2039, 0x203A, 0x2713]

func makeFont(_ file: String, _ size: CGFloat, _ name: String) {
    let provider = CGDataProvider(url: URL(fileURLWithPath: fontDir).appendingPathComponent(file) as CFURL)!
    let font = CTFontCreateWithGraphicsFont(CGFont(provider)!, size, nil, nil)
    let ascent = Int(ceil(CTFontGetAscent(font)))
    let descent = Int(ceil(CTFontGetDescent(font)))
    let lineH = ascent + descent + Int(round(CTFontGetLeading(font)))
    let codes = Array(32...126).map { UInt32($0) } + extraChars

    var glyphRecs = [UInt8](), bitmap = [UInt8]()
    for code in codes {
        var uni = UniChar(code)
        var glyph = CGGlyph(0)
        CTFontGetGlyphsForCharacters(font, &uni, &glyph, 1)
        var adv = CGSize.zero
        var qglyph = CGGlyph(0)
        var q = UniChar(63)
        CTFontGetGlyphsForCharacters(font, &q, &qglyph, 1)
        let synthCheck = code == 0x2713 && glyph == 0
        if glyph == 0 && !synthCheck { glyph = qglyph }
        if synthCheck {
            adv = CGSize(width: size * 0.8, height: 0)
        } else {
            CTFontGetAdvancesForGlyphs(font, .horizontal, &glyph, &adv, 1)
        }
        var bbox = CGRect.zero
        if synthCheck {
            bbox = CGRect(x: size * 0.05, y: 0, width: size * 0.7, height: size * 0.7)
        } else {
            CTFontGetBoundingRectsForGlyphs(font, .horizontal, &glyph, &bbox, 1)
        }
        var w = 0, h = 0, left = 0, top = 0
        let off = bitmap.count
        if !bbox.isEmpty && code != 32 {
            let x0 = Int(floor(bbox.minX)) - 1, y0 = Int(floor(bbox.minY)) - 1
            let x1 = Int(ceil(bbox.maxX)) + 1, y1 = Int(ceil(bbox.maxY)) + 1
            w = x1 - x0; h = y1 - y0; left = x0; top = y1
            let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                                space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
            ctx.setFillColor(gray: 0, alpha: 1)
            ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
            ctx.setFillColor(gray: 1, alpha: 1)
            ctx.setAllowsFontSmoothing(false)
            ctx.setShouldAntialias(true)
            if synthCheck {
                ctx.setStrokeColor(gray: 1, alpha: 1)
                ctx.setLineWidth(size * 0.12)
                ctx.setLineCap(.round)
                ctx.move(to: CGPoint(x: -CGFloat(x0) + size * 0.12, y: -CGFloat(y0) + size * 0.35))
                ctx.addLine(to: CGPoint(x: -CGFloat(x0) + size * 0.32, y: -CGFloat(y0) + size * 0.1))
                ctx.addLine(to: CGPoint(x: -CGFloat(x0) + size * 0.7, y: -CGFloat(y0) + size * 0.65))
                ctx.strokePath()
            } else {
                var pos = CGPoint(x: -CGFloat(x0), y: -CGFloat(y0))
                CTFontDrawGlyphs(font, &glyph, &pos, 1, ctx)
            }
            let p = ctx.data!.bindMemory(to: UInt8.self, capacity: w * h)
            bitmap += Array(UnsafeBufferPointer(start: p, count: w * h))
        }
        let adv16 = Int(round(adv.width * 16))
        glyphRecs += le16(adv16) + [UInt8(w), UInt8(h), UInt8(bitPattern: Int8(left)), UInt8(bitPattern: Int8(top))]
        glyphRecs += le16(0) + le32(off)
    }
    let header = Array("GZF1".utf8) + le16(Int(size)) + le16(lineH) + le16(ascent) + le16(descent) + le16(32) + le16(codes.count)
    save(name, header + glyphRecs + bitmap)
}

// MARK: - Icons (44 x 44)

let iconSize = 44
let iconRadius: CGFloat = 8

func icon(_ name: String, gloss: Bool = true, _ content: (CGContext) -> Void) {
    image("icon_\(name).gzi", iconSize, iconSize) { ctx in
        let r = CGRect(x: 0, y: 0, width: iconSize, height: iconSize)
        ctx.saveGState()
        ctx.addPath(rr(r, iconRadius))
        ctx.clip()
        content(ctx)
        if gloss {
            ctx.saveGState()
            let shine = CGMutablePath()
            shine.addEllipse(in: CGRect(x: -18, y: -44, width: 80, height: 66))
            ctx.addPath(shine)
            ctx.clip()
            gradient(ctx, [(0xFFFFFF, 0.62, 0), (0xFFFFFF, 0.14, 1)], from: CGPoint(x: 0, y: 0), to: CGPoint(x: 0, y: 22))
            ctx.restoreGState()
        }
        ctx.restoreGState()
    }
}

func gearPath(_ c: CGPoint, outer: CGFloat, inner: CGFloat, teeth: Int, hole: CGFloat) -> CGPath {
    let p = CGMutablePath()
    let n = teeth * 4
    for i in 0..<n {
        let a = CGFloat(i) / CGFloat(n) * 2 * .pi
        let r = (i % 4 == 0 || i % 4 == 1) ? outer : inner
        let pt = CGPoint(x: c.x + cos(a) * r, y: c.y + sin(a) * r)
        if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
    }
    p.closeSubpath()
    p.addEllipse(in: CGRect(x: c.x - hole, y: c.y - hole, width: hole * 2, height: hole * 2))
    return p
}

func makeIcons() {
    icon("clock") { ctx in
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 44), 0x3A3A3A, 0x050505)
        let c = CGPoint(x: 22, y: 22)
        fill(ctx, CGPath(ellipseIn: CGRect(x: 4.5, y: 4.5, width: 35, height: 35), transform: nil), col(0xFFFFFF))
        ctx.saveGState()
        ctx.addEllipse(in: CGRect(x: 4.5, y: 4.5, width: 35, height: 35))
        ctx.clip()
        gradient(ctx, [(0xFFFFFF, 1, 0), (0xD8D8D8, 1, 1)], from: CGPoint(x: 0, y: 5), to: CGPoint(x: 0, y: 40))
        ctx.restoreGState()
        ctx.setStrokeColor(col(0x222222))
        ctx.setLineCap(.round)
        for i in 0..<12 {
            let a = CGFloat(i) / 12 * 2 * .pi
            ctx.setLineWidth(i % 3 == 0 ? 1.6 : 0.9)
            ctx.move(to: CGPoint(x: c.x + sin(a) * 14.5, y: c.y - cos(a) * 14.5))
            ctx.addLine(to: CGPoint(x: c.x + sin(a) * 16.5, y: c.y - cos(a) * 16.5))
            ctx.strokePath()
        }
        func hand(_ frac: CGFloat, _ len: CGFloat, _ w: CGFloat, _ color: UInt32) {
            let a = frac * 2 * .pi
            ctx.setStrokeColor(col(color))
            ctx.setLineWidth(w)
            ctx.move(to: CGPoint(x: c.x - sin(a) * 2.5, y: c.y + cos(a) * 2.5))
            ctx.addLine(to: CGPoint(x: c.x + sin(a) * len, y: c.y - cos(a) * len))
            ctx.strokePath()
        }
        hand((10 + 10 / 60) / 12, 8.5, 2.4, 0x111111)
        hand(10 / 60, 12.5, 1.8, 0x111111)
        hand(37 / 60, 13.5, 0.8, 0xD0201A)
        fill(ctx, CGPath(ellipseIn: CGRect(x: 20.6, y: 20.6, width: 2.8, height: 2.8), transform: nil), col(0xD0201A))
    }

    icon("calculator") { ctx in
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 44), 0x6E6E6E, 0x1A1A1A)
        let keys: [(CGFloat, CGFloat, Bool)] = [(6, 6, false), (23, 6, false), (6, 23, false), (23, 23, true)]
        for (i, k) in keys.enumerated() {
            let r = CGRect(x: k.0, y: k.1, width: 15, height: 15)
            fill(ctx, rr(r.insetBy(dx: -0.5, dy: -0.5), 3.5), col(0x000000, 0.6))
            ctx.saveGState()
            ctx.addPath(rr(r, 3))
            ctx.clip()
            if k.2 {
                vgrad(ctx, r, 0xFFB340, 0xE2700C)
            } else {
                vgrad(ctx, r, 0x9A9A9A, 0x4A4A4A)
            }
            ctx.restoreGState()
            ctx.setStrokeColor(col(0xFFFFFF))
            ctx.setLineWidth(2)
            ctx.setLineCap(.round)
            let cx = r.midX, cy = r.midY
            switch i {
            case 0:
                ctx.move(to: CGPoint(x: cx - 4, y: cy)); ctx.addLine(to: CGPoint(x: cx + 4, y: cy))
                ctx.move(to: CGPoint(x: cx, y: cy - 4)); ctx.addLine(to: CGPoint(x: cx, y: cy + 4))
            case 1:
                ctx.move(to: CGPoint(x: cx - 4, y: cy)); ctx.addLine(to: CGPoint(x: cx + 4, y: cy))
            case 2:
                ctx.move(to: CGPoint(x: cx - 3, y: cy - 3)); ctx.addLine(to: CGPoint(x: cx + 3, y: cy + 3))
                ctx.move(to: CGPoint(x: cx + 3, y: cy - 3)); ctx.addLine(to: CGPoint(x: cx - 3, y: cy + 3))
            default:
                ctx.move(to: CGPoint(x: cx - 4, y: cy - 2)); ctx.addLine(to: CGPoint(x: cx + 4, y: cy - 2))
                ctx.move(to: CGPoint(x: cx - 4, y: cy + 2)); ctx.addLine(to: CGPoint(x: cx + 4, y: cy + 2))
            }
            ctx.strokePath()
        }
    }

    icon("notes") { ctx in
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 44), 0xFCF2A6, 0xEED45A)
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 12), 0x8A5A2E, 0x4E2F14)
        ctx.setStrokeColor(col(0xD9B48A, 0.8))
        ctx.setLineWidth(0.8)
        ctx.setLineDash(phase: 0, lengths: [2, 1.5])
        ctx.move(to: CGPoint(x: 0, y: 9.5)); ctx.addLine(to: CGPoint(x: 44, y: 9.5))
        ctx.strokePath()
        ctx.setLineDash(phase: 0, lengths: [])
        fill(ctx, CGPath(rect: CGRect(x: 0, y: 12, width: 44, height: 1.5), transform: nil), col(0x2E1A08, 0.5))
        ctx.setStrokeColor(col(0xB8A060, 0.7))
        ctx.setLineWidth(0.8)
        for y in stride(from: CGFloat(19.5), through: 41, by: 5.5) {
            ctx.move(to: CGPoint(x: 0, y: y)); ctx.addLine(to: CGPoint(x: 44, y: y))
        }
        ctx.strokePath()
        ctx.setStrokeColor(col(0xD86A6A, 0.6))
        ctx.move(to: CGPoint(x: 8.5, y: 13)); ctx.addLine(to: CGPoint(x: 8.5, y: 44))
        ctx.strokePath()
    }

    icon("photos") { ctx in
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 44), 0x3D8FD6, 0xB9E1F7)
        vgrad(ctx, CGRect(x: 0, y: 34, width: 44, height: 10), 0x6DBB4A, 0x3C7D24)
        let c = CGPoint(x: 22, y: 21)
        for i in 0..<14 {
            ctx.saveGState()
            ctx.translateBy(x: c.x, y: c.y)
            ctx.rotate(by: CGFloat(i) / 14 * 2 * .pi)
            let petal = CGPath(ellipseIn: CGRect(x: -2.6, y: -15, width: 5.2, height: 10), transform: nil)
            ctx.addPath(petal)
            ctx.clip()
            gradient(ctx, [(0xFFE24A, 1, 0), (0xF2A516, 1, 1)], from: CGPoint(x: 0, y: -15), to: CGPoint(x: 0, y: -5))
            ctx.restoreGState()
        }
        ctx.saveGState()
        ctx.addEllipse(in: CGRect(x: c.x - 6.5, y: c.y - 6.5, width: 13, height: 13))
        ctx.clip()
        radial(ctx, CGPoint(x: c.x - 1.5, y: c.y - 1.5), 8, 0x7A4E1C, 0x2B1606)
        ctx.restoreGState()
    }

    icon("settings") { ctx in
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 44), 0xE9EBEE, 0x80878F)
        let big = gearPath(CGPoint(x: 21, y: 23), outer: 15.5, inner: 12.5, teeth: 12, hole: 5)
        ctx.addPath(big)
        ctx.setFillColor(col(0x1E2328, 0.35))
        ctx.saveGState(); ctx.translateBy(x: 0, y: 1); ctx.addPath(big); ctx.fillPath(using: .evenOdd); ctx.restoreGState()
        ctx.saveGState()
        ctx.addPath(big)
        ctx.clip(using: .evenOdd)
        gradient(ctx, [(0x7C838B, 1, 0), (0x3B4148, 1, 1)], from: CGPoint(x: 0, y: 8), to: CGPoint(x: 0, y: 38))
        ctx.restoreGState()
        let inner = CGPath(ellipseIn: CGRect(x: 12.5, y: 14.5, width: 17, height: 17), transform: nil)
        ctx.addPath(inner)
        ctx.addEllipse(in: CGRect(x: 16, y: 18, width: 10, height: 10))
        ctx.setFillColor(col(0x9AA1A9))
        ctx.fillPath(using: .evenOdd)
    }

    icon("files") { ctx in
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 44), 0x5AA0F0, 0x1A56B8)
        // Back of the folder with its tab, a sheet of paper, then the front.
        let back = CGMutablePath()
        back.addPath(rr(CGRect(x: 7, y: 10, width: 13, height: 7), 2))
        back.addPath(rr(CGRect(x: 7, y: 13, width: 30, height: 22), 2.5))
        fill(ctx, back, col(0xB97E22))
        fill(ctx, CGPath(rect: CGRect(x: 10, y: 15.5, width: 24, height: 10), transform: nil), col(0xFFFFFF))
        let front = CGRect(x: 6, y: 18, width: 32, height: 17)
        fill(ctx, rr(front.offsetBy(dx: 0, dy: 1), 2.5), col(0x000000, 0.3))
        ctx.saveGState()
        ctx.addPath(rr(front, 2.5))
        ctx.clip()
        vgrad(ctx, front, 0xFFDB7A, 0xE9A93C)
        ctx.restoreGState()
    }

    icon("web") { ctx in
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 44), 0xF4F6F8, 0xB9C2CC)
        let c = CGPoint(x: 22, y: 22)
        let disc = CGRect(x: 6, y: 6, width: 32, height: 32)
        fill(ctx, CGPath(ellipseIn: disc.offsetBy(dx: 0, dy: 1), transform: nil), col(0x1E2328, 0.3))
        ctx.saveGState()
        ctx.addEllipse(in: disc)
        ctx.clip()
        gradient(ctx, [(0x5CC0F8, 1, 0), (0x0F5FC9, 1, 1)], from: CGPoint(x: 0, y: 6), to: CGPoint(x: 0, y: 38))
        ctx.restoreGState()
        ctx.setStrokeColor(col(0xFFFFFF))
        ctx.setLineWidth(1.2)
        ctx.strokeEllipse(in: disc.insetBy(dx: 2.5, dy: 2.5))
        for i in 0..<16 {
            let a = CGFloat(i) / 16 * 2 * .pi
            let r0: CGFloat = i % 4 == 0 ? 9.5 : 11
            ctx.setLineWidth(i % 4 == 0 ? 1.2 : 0.7)
            ctx.move(to: CGPoint(x: c.x + sin(a) * r0, y: c.y - cos(a) * r0))
            ctx.addLine(to: CGPoint(x: c.x + sin(a) * 12.5, y: c.y - cos(a) * 12.5))
            ctx.strokePath()
        }
        // The needle points north-east: red tip, white tail.
        let tip = CGPoint(x: c.x + 9.5, y: c.y - 9.5), tail = CGPoint(x: c.x - 9.5, y: c.y + 9.5)
        let side1 = CGPoint(x: c.x - 2.2, y: c.y - 2.2), side2 = CGPoint(x: c.x + 2.2, y: c.y + 2.2)
        let red = CGMutablePath(); red.addLines(between: [tip, side1, side2]); red.closeSubpath()
        let white = CGMutablePath(); white.addLines(between: [tail, side1, side2]); white.closeSubpath()
        fill(ctx, red, col(0xE8302A))
        fill(ctx, white, col(0xFFFFFF))
        fill(ctx, CGPath(ellipseIn: CGRect(x: c.x - 1.3, y: c.y - 1.3, width: 2.6, height: 2.6), transform: nil), col(0x2A2A2A))
    }
    icon("calendar") { ctx in
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 44), 0xFFFFFF, 0xDCDCDC)
        vgrad(ctx, CGRect(x: 0, y: 0, width: 44, height: 13), 0xF0544A, 0xB51D15)
        fill(ctx, CGPath(rect: CGRect(x: 0, y: 13, width: 44, height: 1), transform: nil), col(0x7A0F0A, 0.5))
    }
}

// MARK: - Wallpapers and photos (240 x 320)

let W = 240, H = 320
let full = CGRect(x: 0, y: 0, width: 240, height: 320)

func mountains(_ ctx: CGContext, base: CGFloat, amp: CGFloat, seed: UInt32, color: CGColor) {
    var rng = LCG(s: seed)
    let p = CGMutablePath()
    p.move(to: CGPoint(x: 0, y: 320))
    var x: CGFloat = 0
    p.addLine(to: CGPoint(x: 0, y: base))
    while x < 240 {
        let dx = 18 + rng.next() * 30
        let peak = base - amp * (0.3 + rng.next() * 0.7)
        p.addLine(to: CGPoint(x: x + dx / 2, y: peak))
        x += dx
        p.addLine(to: CGPoint(x: x, y: base - amp * rng.next() * 0.3))
    }
    p.addLine(to: CGPoint(x: 240, y: 320))
    p.closeSubpath()
    fill(ctx, p, color)
}

func makeWallpapers() {
    image("wall_aqua.gzi", W, H, alpha: false) { ctx in
        gradient(ctx, [(0x041733, 1, 0), (0x0D3E7A, 1, 0.55), (0x2A86C9, 1, 1)], from: .zero, to: CGPoint(x: 0, y: 320))
        for i in 0..<6 {
            let p = CGMutablePath()
            let y0 = 120 + CGFloat(i) * 26
            p.move(to: CGPoint(x: -20, y: y0))
            p.addCurve(to: CGPoint(x: 260, y: y0 - 40 + CGFloat(i) * 6),
                       control1: CGPoint(x: 60, y: y0 - 70), control2: CGPoint(x: 170, y: y0 + 50))
            p.addLine(to: CGPoint(x: 260, y: y0 + 20))
            p.addCurve(to: CGPoint(x: -20, y: y0 + 14),
                       control1: CGPoint(x: 170, y: y0 + 70), control2: CGPoint(x: 60, y: y0 - 40))
            p.closeSubpath()
            fill(ctx, p, col(0x9ED8FF, 0.05 + CGFloat(i % 3) * 0.025))
        }
        radial(ctx, CGPoint(x: 70, y: 60), 160, 0x6FB8FF, 0x6FB8FF, 0.25, 0)
    }

    image("wall_dusk.gzi", W, H, alpha: false) { ctx in
        gradient(ctx, [(0x15123A, 1, 0), (0x4B2860, 1, 0.35), (0xC4566A, 1, 0.68), (0xF6B36A, 1, 0.9)],
                 from: .zero, to: CGPoint(x: 0, y: 320))
        var rng = LCG(s: 7)
        for _ in 0..<60 {
            let x = rng.next() * 240, y = rng.next() * 120, s = 0.4 + rng.next() * 0.9
            fill(ctx, CGPath(ellipseIn: CGRect(x: x, y: y, width: s, height: s), transform: nil), col(0xFFFFFF, 0.4 + rng.next() * 0.5))
        }
        mountains(ctx, base: 268, amp: 70, seed: 3, color: col(0x3A1F3D))
        mountains(ctx, base: 292, amp: 46, seed: 11, color: col(0x1E1024))
    }

    image("wall_meadow.gzi", W, H, alpha: false) { ctx in
        gradient(ctx, [(0x2C7FBF, 1, 0), (0x9ED3F2, 1, 0.55), (0xE6F5D8, 1, 0.62)], from: .zero, to: CGPoint(x: 0, y: 320))
        var rng = LCG(s: 21)
        for _ in 0..<5 {
            let x = rng.next() * 220, y = 40 + rng.next() * 100, w = 50 + rng.next() * 60
            for k in 0..<4 {
                let cx = x + CGFloat(k) * w / 4
                fill(ctx, CGPath(ellipseIn: CGRect(x: cx, y: y - CGFloat(k % 2) * 8, width: w / 2.2, height: 18), transform: nil),
                     col(0xFFFFFF, 0.55))
            }
        }
        let hill = CGMutablePath()
        hill.move(to: CGPoint(x: 0, y: 220))
        hill.addCurve(to: CGPoint(x: 240, y: 200), control1: CGPoint(x: 80, y: 180), control2: CGPoint(x: 160, y: 250))
        hill.addLine(to: CGPoint(x: 240, y: 320)); hill.addLine(to: CGPoint(x: 0, y: 320)); hill.closeSubpath()
        ctx.saveGState(); ctx.addPath(hill); ctx.clip()
        gradient(ctx, [(0x8FD15A, 1, 0), (0x2F7A1F, 1, 1)], from: CGPoint(x: 0, y: 190), to: CGPoint(x: 0, y: 320))
        ctx.restoreGState()
    }
}

func makePhotos() {
    image("photo_1.gzi", W, H, alpha: false) { ctx in      // sunset over water
        gradient(ctx, [(0x2B1B4F, 1, 0), (0xD9566A, 1, 0.45), (0xFFC46B, 1, 0.6)], from: .zero, to: CGPoint(x: 0, y: 192))
        radial(ctx, CGPoint(x: 120, y: 182), 70, 0xFFF2B0, 0xFFB050, 1, 0)
        fill(ctx, CGPath(ellipseIn: CGRect(x: 96, y: 160, width: 48, height: 48), transform: nil), col(0xFFE9A0))
        vgrad(ctx, CGRect(x: 0, y: 190, width: 240, height: 130), 0x40305A, 0x0C0B22)
        var rng = LCG(s: 5)
        for i in 0..<26 {
            let y = 194 + CGFloat(i) * 4.8
            let w = 60 - CGFloat(i) * 1.6 + rng.next() * 16
            fill(ctx, CGPath(rect: CGRect(x: 120 - w / 2 + (rng.next() - 0.5) * 8, y: y, width: w, height: 1.4), transform: nil),
                 col(0xFFD27A, 0.75 - CGFloat(i) * 0.025))
        }
    }
    image("photo_2.gzi", W, H, alpha: false) { ctx in      // mountains
        gradient(ctx, [(0x2F7FD0, 1, 0), (0xA9D8F5, 1, 0.6)], from: .zero, to: CGPoint(x: 0, y: 220))
        let peak = CGMutablePath()
        peak.move(to: CGPoint(x: -10, y: 240)); peak.addLine(to: CGPoint(x: 70, y: 120)); peak.addLine(to: CGPoint(x: 100, y: 150))
        peak.addLine(to: CGPoint(x: 150, y: 80)); peak.addLine(to: CGPoint(x: 250, y: 230)); peak.closeSubpath()
        ctx.saveGState(); ctx.addPath(peak); ctx.clip()
        gradient(ctx, [(0x6D7B8C, 1, 0), (0x34404D, 1, 1)], from: CGPoint(x: 0, y: 80), to: CGPoint(x: 0, y: 240))
        let snow = CGMutablePath()
        snow.move(to: CGPoint(x: 150, y: 80)); snow.addLine(to: CGPoint(x: 120, y: 125)); snow.addLine(to: CGPoint(x: 135, y: 118))
        snow.addLine(to: CGPoint(x: 150, y: 132)); snow.addLine(to: CGPoint(x: 165, y: 115)); snow.addLine(to: CGPoint(x: 182, y: 128))
        snow.closeSubpath()
        fill(ctx, snow, col(0xF4F8FC))
        ctx.restoreGState()
        mountains(ctx, base: 270, amp: 50, seed: 9, color: col(0x2E6B2A))
        vgrad(ctx, CGRect(x: 0, y: 270, width: 240, height: 50), 0x3E8A2E, 0x1F4D17)
    }
    image("photo_3.gzi", W, H, alpha: false) { ctx in      // night sky
        gradient(ctx, [(0x02030A, 1, 0), (0x0B1A3A, 1, 0.7), (0x1E3A5F, 1, 1)], from: .zero, to: CGPoint(x: 0, y: 320))
        var rng = LCG(s: 99)
        for _ in 0..<220 {
            let s = rng.next() < 0.9 ? 0.8 : 1.8
            fill(ctx, CGPath(ellipseIn: CGRect(x: rng.next() * 240, y: rng.next() * 260, width: s, height: s), transform: nil),
                 col(0xFFFFFF, 0.3 + rng.next() * 0.7))
        }
        radial(ctx, CGPoint(x: 172, y: 70), 46, 0xFFFDE8, 0xFFFDE8, 0.35, 0)
        fill(ctx, CGPath(ellipseIn: CGRect(x: 156, y: 54, width: 32, height: 32), transform: nil), col(0xFFFBE0))
        fill(ctx, CGPath(ellipseIn: CGRect(x: 165, y: 50, width: 30, height: 30), transform: nil), col(0x081530))
        mountains(ctx, base: 300, amp: 40, seed: 4, color: col(0x050810))
    }
    image("photo_4.gzi", W, H, alpha: false) { ctx in      // beach
        gradient(ctx, [(0x3BA4E6, 1, 0), (0xBCE6FA, 1, 1)], from: .zero, to: CGPoint(x: 0, y: 170))
        fill(ctx, CGPath(ellipseIn: CGRect(x: 168, y: 34, width: 34, height: 34), transform: nil), col(0xFFF6C2))
        vgrad(ctx, CGRect(x: 0, y: 170, width: 240, height: 70), 0x1B8DB8, 0x48C3D9)
        let sand = CGMutablePath()
        sand.move(to: CGPoint(x: 0, y: 236))
        sand.addCurve(to: CGPoint(x: 240, y: 250), control1: CGPoint(x: 90, y: 222), control2: CGPoint(x: 170, y: 262))
        sand.addLine(to: CGPoint(x: 240, y: 320)); sand.addLine(to: CGPoint(x: 0, y: 320)); sand.closeSubpath()
        ctx.saveGState(); ctx.addPath(sand); ctx.clip()
        gradient(ctx, [(0xF5E3B5, 1, 0), (0xD9B97A, 1, 1)], from: CGPoint(x: 0, y: 230), to: CGPoint(x: 0, y: 320))
        ctx.restoreGState()
        ctx.setStrokeColor(col(0xFFFFFF, 0.8)); ctx.setLineWidth(2)
        ctx.move(to: CGPoint(x: 0, y: 238)); ctx.addCurve(to: CGPoint(x: 240, y: 252), control1: CGPoint(x: 90, y: 224), control2: CGPoint(x: 170, y: 264))
        ctx.strokePath()
    }
    image("photo_5.gzi", W, H, alpha: false) { ctx in      // forest
        gradient(ctx, [(0xCFE8D8, 1, 0), (0x7FB38E, 1, 1)], from: .zero, to: CGPoint(x: 0, y: 320))
        for layer in 0..<3 {
            var rng = LCG(s: UInt32(40 + layer))
            let shade: [UInt32] = [0x5E9470, 0x2F6B43, 0x143D22]
            let baseY = 200 + CGFloat(layer) * 45
            var x: CGFloat = -10
            while x < 250 {
                let h = 80 + rng.next() * 70, w = 26 + rng.next() * 18
                let t = CGMutablePath()
                t.move(to: CGPoint(x: x, y: baseY)); t.addLine(to: CGPoint(x: x + w / 2, y: baseY - h))
                t.addLine(to: CGPoint(x: x + w, y: baseY)); t.closeSubpath()
                fill(ctx, t, col(shade[layer]))
                x += w * 0.7
            }
            fill(ctx, CGPath(rect: CGRect(x: 0, y: baseY, width: 240, height: 320 - baseY), transform: nil), col(shade[layer]))
        }
    }
    image("photo_6.gzi", W, H, alpha: false) { ctx in      // abstract
        gradient(ctx, [(0x1A1A2E, 1, 0), (0x16213E, 1, 1)], from: .zero, to: CGPoint(x: 0, y: 320))
        var rng = LCG(s: 1234)
        let palette: [UInt32] = [0xE94560, 0xF5A623, 0x0F9D9A, 0x7B61FF, 0x4FC3F7]
        for _ in 0..<26 {
            let r = 10 + rng.next() * 50
            let c = palette[Int(rng.next() * CGFloat(palette.count)) % palette.count]
            radial(ctx, CGPoint(x: rng.next() * 240, y: rng.next() * 320), r, c, c, 0.55, 0)
        }
    }
}

// MARK: - UI parts

func makeUI() {
    // Slide-to-unlock knob, 50 x 36.
    image("ui_knob.gzi", 50, 36) { ctx in
        let r = CGRect(x: 0.5, y: 0.5, width: 49, height: 35)
        ctx.saveGState(); ctx.addPath(rr(r, 7)); ctx.clip()
        gradient(ctx, [(0xFFFFFF, 1, 0), (0xE9E9E9, 1, 0.5), (0xC4C4C4, 1, 0.51), (0xDCDCDC, 1, 1)],
                 from: .zero, to: CGPoint(x: 0, y: 36))
        ctx.restoreGState()
        ctx.addPath(rr(r, 7)); ctx.setStrokeColor(col(0x6E6E6E)); ctx.setLineWidth(1); ctx.strokePath()
        let a = CGMutablePath()
        a.move(to: CGPoint(x: 13, y: 15)); a.addLine(to: CGPoint(x: 25, y: 15)); a.addLine(to: CGPoint(x: 25, y: 10))
        a.addLine(to: CGPoint(x: 36, y: 18)); a.addLine(to: CGPoint(x: 25, y: 26)); a.addLine(to: CGPoint(x: 25, y: 21))
        a.addLine(to: CGPoint(x: 13, y: 21)); a.closeSubpath()
        ctx.saveGState(); ctx.addPath(a); ctx.clip()
        gradient(ctx, [(0xA0A0A0, 1, 0), (0x6A6A6A, 1, 1)], from: CGPoint(x: 0, y: 10), to: CGPoint(x: 0, y: 26))
        ctx.restoreGState()
    }

    // Navigation bar back button (3-slice: 12 px point, 1 px body, 6 px cap), 24 px tall.
    func backButton(_ name: String, _ top: UInt32, _ bottom: UInt32) {
        image(name, 30, 24) { ctx in
            let p = CGMutablePath()
            p.move(to: CGPoint(x: 1, y: 12))
            p.addLine(to: CGPoint(x: 10.5, y: 1.5))
            p.addLine(to: CGPoint(x: 25, y: 1.5))
            p.addArc(tangent1End: CGPoint(x: 29, y: 1.5), tangent2End: CGPoint(x: 29, y: 12), radius: 4)
            p.addArc(tangent1End: CGPoint(x: 29, y: 22.5), tangent2End: CGPoint(x: 20, y: 22.5), radius: 4)
            p.addLine(to: CGPoint(x: 10.5, y: 22.5))
            p.closeSubpath()
            ctx.saveGState(); ctx.addPath(p); ctx.clip()
            gradient(ctx, [(top, 1, 0), (bottom, 1, 1)], from: CGPoint(x: 0, y: 1), to: CGPoint(x: 0, y: 23))
            gradient(ctx, [(0xFFFFFF, 0.25, 0), (0xFFFFFF, 0.05, 0.5), (0xFFFFFF, 0, 0.51)], from: CGPoint(x: 0, y: 1), to: CGPoint(x: 0, y: 23))
            ctx.restoreGState()
            ctx.addPath(p); ctx.setStrokeColor(col(0x2A3442, 0.85)); ctx.setLineWidth(1); ctx.strokePath()
        }
    }
    backButton("ui_back.gzi", 0x7E93AF, 0x4A6383)
    backButton("ui_back_dark.gzi", 0x5A5A5A, 0x262626)
    backButton("ui_back_brown.gzi", 0xB08055, 0x6A421F)

    // Tab bar icons (24 x 24 masks).
    func mask(_ name: String, _ draw: (CGContext) -> Void) {
        image(name, 24, 24) { ctx in
            ctx.setFillColor(col(0xFFFFFF)); ctx.setStrokeColor(col(0xFFFFFF))
            draw(ctx)
        }
    }
    mask("tab_world.gzi") { ctx in
        ctx.setLineWidth(1.6)
        ctx.strokeEllipse(in: CGRect(x: 3, y: 3, width: 18, height: 18))
        ctx.strokeEllipse(in: CGRect(x: 8, y: 3, width: 8, height: 18))
        ctx.move(to: CGPoint(x: 3, y: 12)); ctx.addLine(to: CGPoint(x: 21, y: 12))
        ctx.move(to: CGPoint(x: 5, y: 7.5)); ctx.addLine(to: CGPoint(x: 19, y: 7.5))
        ctx.move(to: CGPoint(x: 5, y: 16.5)); ctx.addLine(to: CGPoint(x: 19, y: 16.5))
        ctx.strokePath()
    }
    mask("tab_stopwatch.gzi") { ctx in
        ctx.setLineWidth(1.8)
        ctx.strokeEllipse(in: CGRect(x: 4, y: 5, width: 16, height: 16))
        ctx.fill(CGRect(x: 10, y: 1, width: 4, height: 3))
        ctx.setLineCap(.round)
        ctx.move(to: CGPoint(x: 12, y: 13)); ctx.addLine(to: CGPoint(x: 15.5, y: 8.5))
        ctx.strokePath()
    }
    mask("tab_timer.gzi") { ctx in
        let p = CGMutablePath()
        p.move(to: CGPoint(x: 6, y: 3)); p.addLine(to: CGPoint(x: 18, y: 3)); p.addLine(to: CGPoint(x: 13, y: 12))
        p.addLine(to: CGPoint(x: 18, y: 21)); p.addLine(to: CGPoint(x: 6, y: 21)); p.addLine(to: CGPoint(x: 11, y: 12)); p.closeSubpath()
        ctx.setLineWidth(1.6); ctx.addPath(p); ctx.strokePath()
        let sand = CGMutablePath()
        sand.move(to: CGPoint(x: 8.5, y: 20)); sand.addLine(to: CGPoint(x: 15.5, y: 20)); sand.addLine(to: CGPoint(x: 12, y: 15)); sand.closeSubpath()
        ctx.addPath(sand); ctx.fillPath()
    }
    mask("tab_alarm.gzi") { ctx in
        ctx.setLineWidth(1.8)
        ctx.strokeEllipse(in: CGRect(x: 4, y: 5, width: 16, height: 16))
        ctx.fillEllipse(in: CGRect(x: 2, y: 2, width: 6, height: 6))
        ctx.fillEllipse(in: CGRect(x: 16, y: 2, width: 6, height: 6))
        ctx.setLineCap(.round)
        ctx.move(to: CGPoint(x: 12, y: 13)); ctx.addLine(to: CGPoint(x: 12, y: 8.5))
        ctx.move(to: CGPoint(x: 12, y: 13)); ctx.addLine(to: CGPoint(x: 15, y: 13))
        ctx.strokePath()
    }
}

// MARK: - Main

try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
for size in [9, 10, 11, 12, 13, 14, 17, 24, 44] {
    makeFont("Vera.ttf", CGFloat(size), "sans_r\(size).gzf")
}
for size in [8, 9, 10, 11, 12, 13, 14, 17, 20] {
    makeFont("VeraBd.ttf", CGFloat(size), "sans_b\(size).gzf")
}
makeIcons()
makeWallpapers()
makePhotos()
makeUI()
print("assets written to \(outDir)")
