import AppKit

/// Settings: what the mouse buttons drag, how the wheel zooms, and every key binding. Drawn in
/// the same sticker style as the rest of the app.
@MainActor
final class SettingsWindow: NSWindow {
    private var mouse = MouseSettings.saved()
    var onMouseChange: ((MouseSettings) -> Void)?

    private var gesturePills: [MouseSettings.Gesture: SegmentedPill] = [:]
    private let zoomToward = SegmentedPill(["Pointer", "Centre"], selected: 0, help: "Zoom toward")
    private let wheel = SegmentedPill(
        ["Natural", "Inverted"],
        selected: 0,
        help: "Wheel direction"
    )
    private let speed = StickerSlider(help: "Zoom speed")

    private var keyButtons: [KeyAction: PillButton] = [:]
    private var recording: KeyAction?
    private var monitor: Any?
    private let problem = NSTextField(labelWithString: "")

    init() {
        super.init(
            contentRect: NSRect(x: 0, y: 0, width: 560, height: 720),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        title = "Settings"
        backgroundColor = Palette.paper
        isReleasedWhenClosed = false
        center()

        let content = NSStackView(views: [mouseCard(), keysCard()])
        content.orientation = .vertical
        content.alignment = .leading
        content.spacing = 18
        content.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
        contentView = content
        showMouse()
    }

    // MARK: Mouse

    private func mouseCard() -> NSView {
        var rows: [NSView] = [heading("MOUSE")]
        for gesture in MouseSettings.Gesture.allCases {
            let pill = SegmentedPill(
                MouseSettings.Button.allCases.map(\.title),
                selected: 0,
                help: "The button that drags to \(gesture.title.lowercased())"
            )
            pill.onChange = { [weak self] index in
                self?.editMouse { $0.assign(MouseSettings.Button(rawValue: index)!, to: gesture) }
            }
            gesturePills[gesture] = pill
            rows.append(row(gesture.title, pill))
        }
        zoomToward.onChange = { [weak self] index in
            self?.editMouse { $0.zoomTowardPointer = index == 0 }
        }
        wheel.onChange = { [weak self] index in self?.editMouse { $0.invertWheel = index == 1 } }
        speed.onChange = { [weak self] value in
            // 0.25 to 4 times, evenly in log scale, 1 at the middle.
            self?.editMouse { $0.zoomSpeed = pow(4, value * 2 - 1) }
        }
        rows += [row("Zoom toward", zoomToward), row("Wheel", wheel), row("Zoom speed", speed)]
        return card(rows)
    }

    private func editMouse(_ change: (inout MouseSettings) -> Void) {
        change(&mouse)
        mouse.save()
        showMouse()
        onMouseChange?(mouse)
    }

    private func showMouse() {
        for (gesture, pill) in gesturePills {
            pill.selected = mouse.buttons[gesture]?.rawValue ?? 0
        }
        zoomToward.selected = mouse.zoomTowardPointer ? 0 : 1
        wheel.selected = mouse.invertWheel ? 1 : 0
        speed.value = (log(mouse.zoomSpeed) / log(4) + 1) / 2
    }

    // MARK: Keys

    private func keysCard() -> NSView {
        var rows: [NSView] = [heading("KEYS")]
        for action in KeyAction.allCases {
            let button = PillButton(title: "", help: "Click, then press the new key")
            button.onPress = { [weak self] in self?.record(action) }
            keyButtons[action] = button
            rows.append(row(action.title, button))
        }
        problem.font = Typography.font(Typography.textSmall, .medium)
        problem.textColor = Palette.tomato
        let reset = PillButton(title: "Reset to defaults", help: "Restore every key")
        reset.onPress = { [weak self] in
            KeyMap.shared.resetToDefaults()
            self?.problem.stringValue = ""
            self?.showKeys()
        }
        rows += [problem, reset]
        showKeys()
        return card(rows)
    }

    private func showKeys() {
        for (action, button) in keyButtons {
            button.title =
                recording == action
                ? "Press a key…" : KeyMap.shared.chord(for: action)?.label ?? "—"
        }
    }

    /// Waits for the next key press and binds it, unless it clashes.
    private func record(_ action: KeyAction) {
        stopRecording()
        recording = action
        problem.stringValue = ""
        showKeys()
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard let self, let action = recording, event.window === self,
                let key = event.charactersIgnoringModifiers
            else {
                return event
            }
            let chord = KeyChord(key: key, modifiers: event.modifierFlags)
            switch KeyMap.shared.assign(chord, to: action) {
            case .usedBy(let other)?:
                problem.stringValue = "\(chord.label) already does “\(other.title)”."
            case .reserved?:
                problem.stringValue = "Command shortcuts belong to the menus."
            case .heldNeedsPlainKey?:
                problem.stringValue = "A key that is held must be a single key."
            case nil:
                problem.stringValue = ""
            }
            stopRecording()
            return nil
        }
    }

    private func stopRecording() {
        if let monitor {
            NSEvent.removeMonitor(monitor)
        }
        monitor = nil
        recording = nil
        showKeys()
    }

    override func close() {
        stopRecording()
        super.close()
    }

    // MARK: Layout

    private func heading(_ text: String) -> NSTextField {
        let label = NSTextField(labelWithString: text)
        label.font = Typography.font(Typography.textExtraSmall, .medium)
        label.textColor = Palette.muted
        return label
    }

    private func row(_ title: String, _ control: NSView) -> NSView {
        let label = NSTextField(labelWithString: title)
        label.font = Typography.font(Typography.textSmall, .medium)
        label.textColor = Palette.ink
        label.translatesAutoresizingMaskIntoConstraints = false
        label.widthAnchor.constraint(equalToConstant: 200).isActive = true
        let stack = NSStackView(views: [label, control])
        stack.orientation = .horizontal
        stack.spacing = 10
        return stack
    }

    private func card(_ rows: [NSView]) -> NSView {
        let card = StickerCard()
        let stack = NSStackView(views: rows)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        card.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -16),
            stack.topAnchor.constraint(equalTo: card.topAnchor, constant: 14),
            stack.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -14),
            card.widthAnchor.constraint(equalToConstant: 520),
        ])
        return card
    }
}
