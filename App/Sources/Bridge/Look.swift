import Foundation
import GeometryEngine

/// How the model is drawn, as the user chose it. Remembered between launches.
struct Look: Equatable {
    enum Style: Int, CaseIterable {
        case illustration, metal, clay, blueprint, technical

        var title: String {
            switch self {
            case .illustration: "Illustration"
            case .metal: "Metal"
            case .clay: "Clay"
            case .blueprint: "Blueprint"
            case .technical: "Technical"
            }
        }

        /// Blueprint and Technical draw every part alike.
        var usesColour: Bool { self != .blueprint && self != .technical }
        /// The style casts a shadow unless asked not to.
        var castsShadow: Bool { geo_style_casts_shadow(GeoStyle(rawValue: UInt32(rawValue))) }
        var hasGrain: Bool { self == .illustration || self == .blueprint }
        var canWobble: Bool { self == .illustration }
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
    var colour = Colour.material
    var ghosts = Ghosts.shell
    var shadow = true
    var grain = true
    /// Hand-drawn wobble, in points.
    var wobble: Float = 0

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
        var look = Look()
        if let style = Style(rawValue: defaults.integer(forKey: Key.style)) {
            look.style = style
            look.shadow = style.castsShadow
        }
        if let colour = Colour(rawValue: defaults.integer(forKey: Key.colour)) {
            look.colour = colour
        }
        if let ghosts = Ghosts(rawValue: defaults.integer(forKey: Key.ghosts)) {
            look.ghosts = ghosts
        }
        if defaults.object(forKey: Key.shadow) != nil {
            look.shadow = defaults.bool(forKey: Key.shadow)
        }
        if defaults.object(forKey: Key.grain) != nil {
            look.grain = defaults.bool(forKey: Key.grain)
        }
        look.wobble = defaults.float(forKey: Key.wobble)
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
