import Foundation
import GeometryViewEngine

/// How the model is drawn, as the user chose it. Remembered between launches.
struct Look: Equatable {
    enum Style: Int, CaseIterable {
        case illustration, metal, clay, blueprint, technical, studio

        var title: String {
            switch self {
            case .illustration: "Illustration"
            case .metal: "Metal"
            case .clay: "Clay"
            case .blueprint: "Blueprint"
            case .technical: "Technical"
            case .studio: "Studio"
            }
        }

        /// Blueprint and Technical draw every part alike.
        var usesColour: Bool { self != .blueprint && self != .technical }
        /// The style casts a shadow unless asked not to.
        var castsShadow: Bool { geo_style_casts_shadow(GeoStyle(rawValue: UInt32(rawValue))) }
        var hasGrain: Bool { self == .illustration || self == .blueprint }
        var canWobble: Bool { self == .illustration }
        /// Blueprint and Studio draw on a dark sheet, where the chrome over it turns light.
        var onDark: Bool { geo_style_on_dark(GeoStyle(rawValue: UInt32(rawValue))) }
    }

    enum Colour: Int, CaseIterable {
        case material, parts, mono, file

        var title: String {
            switch self {
            case .material: "Material"
            case .parts: "Parts"
            case .mono: "Mono"
            case .file: "File"
            }
        }

        /// One sentence on where the colours come from.
        var summary: String {
            switch self {
            case .material: "Colours follow the material each name suggests."
            case .parts: "Each part gets a colour of its own."
            case .mono: "Every part in one neutral colour."
            case .file: "The colours stored in the file."
            }
        }
    }

    enum Ghosts: Int, CaseIterable {
        case shell = 1
        case lines = 2
        case both = 3

        var title: String {
            switch self {
            case .shell: "Shell"
            case .lines: "Lines"
            case .both: "Both"
            }
        }
    }

    var style = Style.illustration
    var colour = Colour.parts
    var ghosts = Ghosts.shell
    var shadow = true
    var grain = true
    /// Hand-drawn wobble, in points.
    var wobble: Float = 0

    /// The engine's default look, which every app starts with until the person chooses.
    init() {
        let look = geo_default_look()
        style = Style(rawValue: Int(look.style.rawValue)) ?? .illustration
        colour = Colour(rawValue: Int(look.color_mode.rawValue)) ?? .parts
        ghosts = Ghosts(rawValue: Int(look.ghost_style.rawValue)) ?? .shell
        shadow = look.cast_shadow
        grain = look.grain > 0
        wobble = look.wobble
    }

    static let wobbleSteps: [Float] = [0, 1, 3, 5]

    /// Choosing a style also chooses its usual shadow, as in step_viewer.
    mutating func choose(_ newStyle: Style) {
        style = newStyle
        shadow = newStyle.castsShadow
    }

    var engineLook: GeoLook {
        GeoLook(
            style: GeoStyle(rawValue: UInt32(style.rawValue)),
            color_mode: GeoColorMode(rawValue: UInt32(colour.rawValue)),
            ghost_style: GeoGhostStyle(rawValue: UInt32(ghosts.rawValue)),
            cast_shadow: shadow,
            grain: grain ? 1 : 0,
            wobble: wobble
        )
    }

    // MARK: Remembering

    private enum Key {
        static let style = "look.style"
        static let colour = "look.colour"
        static let ghosts = "look.ghosts"
        static let shadow = "look.shadow"
        static let grain = "look.grain"
        static let wobble = "look.wobble"
    }

    static func saved(in defaults: UserDefaults = .standard) -> Look {
        // A key never saved keeps the default: reading it as 0 would choose Material.
        var look = Look()
        let saved = { (key: String) in defaults.object(forKey: key) == nil ? nil : defaults.integer(forKey: key) }
        if let style = saved(Key.style).flatMap(Style.init(rawValue:)) {
            look.style = style
            look.shadow = style.castsShadow
        }
        if let colour = saved(Key.colour).flatMap(Colour.init(rawValue:)) {
            look.colour = colour
        }
        if let ghosts = saved(Key.ghosts).flatMap(Ghosts.init(rawValue:)) {
            look.ghosts = ghosts
        }
        if defaults.object(forKey: Key.shadow) != nil {
            look.shadow = defaults.bool(forKey: Key.shadow)
        }
        if defaults.object(forKey: Key.grain) != nil {
            look.grain = defaults.bool(forKey: Key.grain)
        }
        if defaults.object(forKey: Key.wobble) != nil {
            look.wobble = defaults.float(forKey: Key.wobble)
        }
        return look
    }

    func save(in defaults: UserDefaults = .standard) {
        defaults.set(style.rawValue, forKey: Key.style)
        defaults.set(colour.rawValue, forKey: Key.colour)
        defaults.set(ghosts.rawValue, forKey: Key.ghosts)
        defaults.set(shadow, forKey: Key.shadow)
        defaults.set(grain, forKey: Key.grain)
        defaults.set(wobble, forKey: Key.wobble)
    }
}
