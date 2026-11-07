import AppKit

/// The style picker: how the model is drawn, what decides its colours and how ghosts look. It
/// opens above the toolbar and every row is also in the View menu.
@MainActor
final class LookCard: StickerCard {
    private let style = SegmentedPill(
        Look.Style.allCases.map(\.title),
        selected: 0,
        help: "Drawing style"
    )
    private let colour = SegmentedPill(
        Look.Colour.allCases.map(\.title),
        selected: 0,
        help: "What decides each part's colour"
    )
    private let ghosts = SegmentedPill(
        Look.Ghosts.allCases.map(\.title),
        selected: 0,
        help: "How ghosted parts look"
    )
    private let castShadow = SegmentedPill(["On", "Off"], selected: 0, help: "Cast shadow")
    private let grain = SegmentedPill(["On", "Off"], selected: 0, help: "Paper grain")
    private let wobble = SegmentedPill(
        ["Off", "Light", "More", "Most"],
        selected: 0,
        help: "Hand-drawn wobble"
    )
    private let colourNote = NSTextField(labelWithString: "")
    private var rows: [String: NSView] = [:]

    private(set) var look = Look()
    var onChange: ((Look) -> Void)?

    init() {
        super.init(frame: .zero)
        let title = NSTextField(labelWithString: "LOOK")
        title.font = Typography.font(Typography.textExtraSmall, .medium)
        title.textColor = Palette.muted
        colourNote.font = Typography.font(Typography.textExtraSmall)
        colourNote.textColor = Palette.muted

        let stack = NSStackView(views: [
            title,
            row("Style", style),
            row("Colour", colour),
            colourNote,
            row("Ghosts", ghosts),
            row("Shadow", castShadow),
            row("Grain", grain),
            row("Hand-drawn", wobble),
        ])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -16),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 14),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -16),
        ])

        style.onChange = { [weak self] index in
            self?.edit { $0.choose(Look.Style.allCases[index]) }
        }
        colour.onChange = { [weak self] index in
            self?.edit { $0.colour = Look.Colour.allCases[index] }
        }
        ghosts.onChange = { [weak self] index in
            self?.edit { $0.ghosts = Look.Ghosts.allCases[index] }
        }
        castShadow.onChange = { [weak self] index in self?.edit { $0.shadow = index == 0 } }
        grain.onChange = { [weak self] index in self?.edit { $0.grain = index == 0 } }
        wobble.onChange = { [weak self] index in
            self?.edit { $0.wobble = Look.wobbleSteps[index] }
        }
    }

    private func row(_ label: String, _ control: NSView) -> NSView {
        let text = NSTextField(labelWithString: label)
        text.font = Typography.font(Typography.textSmall, .medium)
        text.textColor = Palette.ink
        text.translatesAutoresizingMaskIntoConstraints = false
        text.widthAnchor.constraint(equalToConstant: 92).isActive = true
        let row = NSStackView(views: [text, control])
        row.orientation = .horizontal
        row.spacing = 8
        rows[label] = row
        return row
    }

    private func edit(_ change: (inout Look) -> Void) {
        var next = look
        change(&next)
        show(next)
        onChange?(next)
    }

    /// Shows `newLook` without reporting it, for changes made elsewhere, such as the menu.
    func show(_ newLook: Look) {
        look = newLook
        style.selected = newLook.style.rawValue
        colour.selected = newLook.colour.rawValue
        ghosts.selected = Look.Ghosts.allCases.firstIndex(of: newLook.ghosts) ?? 0
        castShadow.selected = newLook.shadow ? 0 : 1
        grain.selected = newLook.grain ? 0 : 1
        wobble.selected = Look.wobbleSteps.firstIndex(of: newLook.wobble) ?? 0
        // Only what the style uses is offered.
        colour.isHidden = !newLook.style.usesColour
        colourNote.stringValue = "\(newLook.style.title) draws every part alike."
        colourNote.isHidden = newLook.style.usesColour
        rows["Colour"]?.isHidden = !newLook.style.usesColour
        rows["Grain"]?.isHidden = !newLook.style.hasGrain
        rows["Hand-drawn"]?.isHidden = !newLook.style.canWobble
    }
}
