import AppKit

/// Type styles. IBM Plex Mono is bundled, and the system monospaced face is only a safety net.
enum Typography {
    enum Weight {
        case regular, medium, semibold, bold

        fileprivate var postScriptName: String {
            switch self {
            case .regular: "IBMPlexMono"
            case .medium: "IBMPlexMono-Medm"
            case .semibold: "IBMPlexMono-SmBld"
            case .bold: "IBMPlexMono-Bold"
            }
        }

        fileprivate var systemWeight: NSFont.Weight {
            switch self {
            case .regular: .regular
            case .medium: .medium
            case .semibold: .semibold
            case .bold: .bold
            }
        }
    }

    static let textExtraSmall: CGFloat = 10.5
    static let textSmall: CGFloat = 11.5
    static let textMedium: CGFloat = 12.5
    static let textLarge: CGFloat = 13.5
    static let textHeading: CGFloat = 15
    static let textStat: CGFloat = 26

    static func font(_ size: CGFloat, _ weight: Weight = .regular) -> NSFont {
        NSFont(name: weight.postScriptName, size: size)
            ?? NSFont.monospacedSystemFont(ofSize: size, weight: weight.systemWeight)
    }
}
