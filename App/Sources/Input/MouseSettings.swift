import Foundation
import GeometryEngine

/// What each mouse button drags, and how the wheel zooms. Remembered across launches.
struct MouseSettings: Equatable {
    enum Button: Int, CaseIterable {
        case left, middle, right

        var title: String {
            switch self {
            case .left: "Left"
            case .middle: "Middle"
            case .right: "Right"
            }
        }

        var engineButton: GeoPointerButton {
            switch self {
            case .left: GeoButtonLeft
            case .middle: GeoButtonMiddle
            case .right: GeoButtonRight
            }
        }
    }

    enum Gesture: Int, CaseIterable {
        case rotate, pan, dolly

        var title: String {
            switch self {
            case .rotate: "Rotate"
            case .pan: "Pan"
            case .dolly: "Dolly"
            }
        }
    }

    /// The button each gesture drags with: every gesture has its own.
    var buttons: [Gesture: Button] = [.rotate: .left, .pan: .middle, .dolly: .right]
    var zoomTowardPointer = true
    var invertWheel = false
    var zoomSpeed: Double = 1

    /// Gives `gesture` the button `button`, swapping with the gesture that had it.
    mutating func assign(_ button: Button, to gesture: Gesture) {
        if let other = buttons.first(where: { $0.value == button })?.key, other != gesture {
            buttons[other] = buttons[gesture]
        }
        buttons[gesture] = button
    }

    var engineSettings: GeoMouseSettings {
        var settings = geo_default_mouse_settings()
        var rows: [GeoMouseBinding] = Gesture.allCases.map { gesture in
            GeoMouseBinding(
                button: buttons[gesture]?.engineButton.rawValue ?? 0,
                modifiers: 0,
                gesture: GeoGesture(rawValue: UInt32(gesture.rawValue + 1))
            )
        }
        // A MacBook has no middle button, so Shift or Space with the rotate button pans.
        if let rotate = buttons[.rotate]?.engineButton.rawValue {
            for modifier in [UInt32(GeoModifierShift), UInt32(GeoModifierSpace)] {
                rows.append(
                    GeoMouseBinding(button: rotate, modifiers: modifier, gesture: GeoGesturePan)
                )
            }
        }
        withUnsafeMutableBytes(of: &settings.bindings) { raw in
            let slots = raw.bindMemory(to: GeoMouseBinding.self)
            for (index, row) in rows.prefix(slots.count).enumerated() {
                slots[index] = row
            }
        }
        settings.binding_count = UInt32(min(rows.count, Int(GEO_MAX_MOUSE_BINDINGS)))
        settings.zoom_toward_pointer = zoomTowardPointer
        settings.invert_wheel = invertWheel
        settings.zoom_speed = Float(zoomSpeed)
        return settings
    }

    // MARK: Remembering

    static func saved(in defaults: UserDefaults = .standard) -> MouseSettings {
        var settings = MouseSettings()
        for gesture in Gesture.allCases {
            if let value = defaults.object(forKey: "mouse.\(gesture)") as? Int,
                let button = Button(rawValue: value)
            {
                settings.buttons[gesture] = button
            }
        }
        if defaults.object(forKey: "mouse.zoomTowardPointer") != nil {
            settings.zoomTowardPointer = defaults.bool(forKey: "mouse.zoomTowardPointer")
        }
        settings.invertWheel = defaults.bool(forKey: "mouse.invertWheel")
        let speed = defaults.double(forKey: "mouse.zoomSpeed")
        settings.zoomSpeed = speed > 0 ? speed : 1
        return settings
    }

    func save(in defaults: UserDefaults = .standard) {
        for (gesture, button) in buttons {
            defaults.set(button.rawValue, forKey: "mouse.\(gesture)")
        }
        defaults.set(zoomTowardPointer, forKey: "mouse.zoomTowardPointer")
        defaults.set(invertWheel, forKey: "mouse.invertWheel")
        defaults.set(zoomSpeed, forKey: "mouse.zoomSpeed")
    }
}
