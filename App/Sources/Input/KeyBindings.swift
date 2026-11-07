import AppKit
import GeometryEngine

/// What keys do in the viewport. Every binding is data here, so settings can edit them later.
enum KeyCommand {
    case perform(GeoAction)
    case selectionMode(GeoSelectionMode)
}

struct KeyBinding {
    let key: String
    let modifiers: NSEvent.ModifierFlags
    let command: KeyCommand
}

/// A key that changes what clicks do for as long as it is held.
struct HeldBinding {
    let key: String
    let tool: GeoTool
}

enum KeyBindings {
    static let pressed: [KeyBinding] = [
        KeyBinding(key: "f", modifiers: [], command: .perform(GeoActionFit)),
        KeyBinding(key: "p", modifiers: [], command: .perform(GeoActionToggleProjection)),
        KeyBinding(key: "\u{1B}", modifiers: [], command: .perform(GeoActionClearSelection)),
        KeyBinding(key: "h", modifiers: .shift, command: .perform(GeoActionShowAll)),
        KeyBinding(key: "g", modifiers: .shift, command: .perform(GeoActionUnghostAll)),
        KeyBinding(key: "i", modifiers: [], command: .perform(GeoActionIsolate)),
        KeyBinding(key: "1", modifiers: [], command: .selectionMode(GeoSelectFaces)),
        KeyBinding(key: "2", modifiers: [], command: .selectionMode(GeoSelectBodies)),
        KeyBinding(key: "3", modifiers: [], command: .selectionMode(GeoSelectParts)),
    ]

    static let held: [HeldBinding] = [
        HeldBinding(key: "h", tool: GeoToolHide),
        HeldBinding(key: "g", tool: GeoToolGhost),
    ]

    /// Modifiers that decide a binding. Caps Lock and the function key never do.
    static let relevant: NSEvent.ModifierFlags = [.shift, .control, .option, .command]
}
