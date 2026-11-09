import AppKit
import GeometryViewEngine

/// What a key press does in the viewport.
enum KeyCommand {
    case perform(GeoAction)
    case selectionMode(GeoSelectionMode)
    case toggleProjection
    case showShortcuts
    case toggleSection
}

/// Everything a key can be bound to, with a stable name for saving.
enum KeyAction: String, CaseIterable {
    case holdHide, holdGhost
    case fit, toggleProjection, clearSelection, isolate, showAll, unghostAll
    case faceMode, bodyMode, partMode
    case section, seeThrough, shortcuts

    var title: String {
        switch self {
        case .holdHide: "Hold to hide"
        case .holdGhost: "Hold to ghost"
        case .fit: "Fit to view"
        case .toggleProjection: "Switch projection"
        case .clearSelection: "Clear the selection"
        case .isolate: "Isolate the selection"
        case .showAll: "Show everything hidden"
        case .unghostAll: "Unghost everything"
        case .faceMode: "Pick faces"
        case .bodyMode: "Pick bodies"
        case .partMode: "Pick parts"
        case .section: "Section"
        case .seeThrough: "See through"
        case .shortcuts: "Show shortcuts"
        }
    }

    /// The tool a held key selects, for actions that last while the key is down.
    var heldTool: GeoTool? {
        switch self {
        case .holdHide: GeoToolHide
        case .holdGhost: GeoToolGhost
        default: nil
        }
    }

    var command: KeyCommand? {
        switch self {
        case .holdHide, .holdGhost: nil
        case .fit: .perform(GeoActionFit)
        case .toggleProjection: .toggleProjection
        case .clearSelection: .perform(GeoActionClearSelection)
        case .isolate: .perform(GeoActionIsolate)
        case .showAll: .perform(GeoActionShowAll)
        case .unghostAll: .perform(GeoActionUnghostAll)
        case .faceMode: .selectionMode(GeoSelectFaces)
        case .bodyMode: .selectionMode(GeoSelectBodies)
        case .partMode: .selectionMode(GeoSelectParts)
        case .section: .toggleSection
        case .seeThrough: .perform(GeoActionToggleSeeThrough)
        case .shortcuts: .showShortcuts
        }
    }
}

/// A key with the modifiers that must be down with it.
struct KeyChord: Equatable {
    let key: String  // as typed without modifiers, lower case; "\u{1B}" is Escape
    let modifiers: NSEvent.ModifierFlags

    /// Modifiers that decide a binding. Caps Lock and the function key never do.
    static let relevant: NSEvent.ModifierFlags = [.shift, .control, .option, .command]

    init(key: String, modifiers: NSEvent.ModifierFlags = []) {
        self.key = key.lowercased()
        self.modifiers = modifiers.intersection(Self.relevant)
    }

    /// How a key cap shows it, such as "⇧H".
    var label: String {
        var text = ""
        if modifiers.contains(.control) { text += "⌃" }
        if modifiers.contains(.option) { text += "⌥" }
        if modifiers.contains(.shift) { text += "⇧" }
        if modifiers.contains(.command) { text += "⌘" }
        switch key {
        case "\u{1B}": text += "Esc"
        case " ": text += "Space"
        default: text += key.uppercased()
        }
        return text
    }

    fileprivate var saved: String { "\(modifiers.rawValue):\(key)" }

    fileprivate init?(saved: String) {
        guard let colon = saved.firstIndex(of: ":"),
            let raw = UInt(saved[..<colon])
        else {
            return nil
        }
        self.init(
            key: String(saved[saved.index(after: colon)...]),
            modifiers: NSEvent.ModifierFlags(rawValue: raw)
        )
    }
}

/// The keys in use. Every binding is data, edited in Settings and remembered across launches.
@MainActor
final class KeyMap {
    static let shared = KeyMap()

    static let defaults: [KeyAction: KeyChord] = [
        .holdHide: KeyChord(key: "h"),
        .holdGhost: KeyChord(key: "g"),
        .fit: KeyChord(key: "f"),
        .toggleProjection: KeyChord(key: "p"),
        .clearSelection: KeyChord(key: "\u{1B}"),
        .isolate: KeyChord(key: "i"),
        .showAll: KeyChord(key: "h", modifiers: .shift),
        .unghostAll: KeyChord(key: "g", modifiers: .shift),
        .faceMode: KeyChord(key: "1"),
        .bodyMode: KeyChord(key: "2"),
        .partMode: KeyChord(key: "3"),
        .section: KeyChord(key: "s"),
        .seeThrough: KeyChord(key: "t"),
        .shortcuts: KeyChord(key: "?", modifiers: .shift),
    ]

    private(set) var chords: [KeyAction: KeyChord]
    private static let key = "keys"
    var onChange: (() -> Void)?

    private init() {
        chords = Self.defaults
        let saved = UserDefaults.standard.dictionary(forKey: Self.key) as? [String: String] ?? [:]
        for (name, value) in saved {
            if let action = KeyAction(rawValue: name), let chord = KeyChord(saved: value) {
                chords[action] = chord
            }
        }
    }

    func chord(for action: KeyAction) -> KeyChord? { chords[action] }

    /// The action a key press means, if any.
    func action(for chord: KeyChord) -> KeyAction? {
        chords.first(where: { $0.value == chord })?.key
    }

    enum Problem: Equatable {
        case usedBy(KeyAction)
        case reserved  // Command shortcuts belong to the menus
        case heldNeedsPlainKey  // a held key cannot need modifiers, which would be released apart
    }

    /// Binds `chord` to `action`, unless that would clash. Returns what is wrong, or nothing.
    @discardableResult
    func assign(_ chord: KeyChord, to action: KeyAction) -> Problem? {
        if chord.modifiers.contains(.command) {
            return .reserved
        }
        if action.heldTool != nil && !chord.modifiers.isEmpty {
            return .heldNeedsPlainKey
        }
        if let other = self.action(for: chord), other != action {
            return .usedBy(other)
        }
        chords[action] = chord
        save()
        return nil
    }

    func resetToDefaults() {
        chords = Self.defaults
        save()
    }

    private func save() {
        UserDefaults.standard.set(
            chords.reduce(into: [String: String]()) { $0[$1.key.rawValue] = $1.value.saved },
            forKey: Self.key
        )
        onChange?()
    }
}
