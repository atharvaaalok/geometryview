import AppKit

/// Colour tokens of the printed-figure look. Each accent has one fixed meaning: cobalt is selection,
/// cream is hover, tomato is about to hide, and mustard is progress.
enum Palette {
    static let paper = NSColor(hex: 0xF2EFE7)
    static let sheet = NSColor(hex: 0xF7F5EE)
    static let sheetSunk = NSColor(hex: 0xECE8DD)
    static let ink = NSColor(hex: 0x231E16)
    static let inkSoft = NSColor(hex: 0x3A342A)
    static let muted = NSColor(hex: 0x6F685C)
    static let faint = NSColor(hex: 0x9A9284)
    static let rule = NSColor(hex: 0xDDD6C8)
    static let cream = NSColor(hex: 0xFFF4D6)
    /// Half cream, for hover where cream already marks the chosen option.
    static let creamWash = NSColor(hex: 0xFBF5E2)
    static let shadowBlue = NSColor(hex: 0x41538F)
    static let mustard = NSColor(hex: 0xF4B829)
    static let tomato = NSColor(hex: 0xEF6A4C)
    static let mint = NSColor(hex: 0x93D3B0)
    static let blush = NSColor(hex: 0xF3A6BB)
    static let cobalt = NSColor(hex: 0x76A0E2)
    static let cobaltMid = NSColor(hex: 0x5A82C6)
    static let cobaltWash = NSColor(hex: 0xDDE6F4)
    static let tomatoWash = NSColor(hex: 0xF8DDD3)
    // Text drawn straight on Blueprint's dark sheet.
    static let inkOnDark = NSColor(hex: 0xEEF4FF)
    static let mutedOnDark = NSColor(hex: 0xA9BEDF)
}

extension NSColor {
    /// Creates an sRGB colour, so hex values match the reference exactly.
    fileprivate convenience init(hex: UInt32) {
        self.init(
            srgbRed: CGFloat((hex >> 16) & 0xFF) / 255,
            green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255,
            alpha: 1
        )
    }
}
