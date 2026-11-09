import AppKit
import GeometryViewEngine

/// Cuts across the model's axes: for each axis, whether it is cut, where, and which side goes.
@MainActor
final class SectionCard: StickerCard {
    private struct Row {
        let toggle: SegmentedPill
        let position: StickerSlider
        let side: SegmentedPill
    }

    private var rows: [Row] = []
    private(set) var section = GeoSection()
    var onChange: ((GeoSection) -> Void)?

    init() {
        super.init()
        let title = NSTextField(labelWithString: "SECTION")
        title.font = Typography.font(Typography.textExtraSmall, .medium)
        title.textColor = Palette.muted
        var views: [NSView] = [title]
        for (axis, name) in ["X", "Y", "Z"].enumerated() {
            let label = NSTextField(labelWithString: name)
            label.font = Typography.font(Typography.textSmall, .semibold)
            label.textColor = Palette.ink
            label.translatesAutoresizingMaskIntoConstraints = false
            label.widthAnchor.constraint(equalToConstant: 14).isActive = true
            let row = Row(
                toggle: SegmentedPill(["Off", "Cut"], selected: 0, help: "Cut across \(name)"),
                position: StickerSlider(help: "Where to cut across \(name)"),
                side: SegmentedPill(["+", "−"], selected: 0, help: "Which side to remove")
            )
            row.toggle.onChange = { [weak self] index in
                self?.edit { $0.setEnabled(axis, index == 1) }
            }
            row.position.onChange = { [weak self] value in
                self?.edit { $0.setPosition(axis, Float(value)) }
            }
            row.side.onChange = { [weak self] index in
                self?.edit { $0.setFlip(axis, index == 1) }
            }
            let stack = NSStackView(views: [label, row.toggle, row.position, row.side])
            stack.orientation = .horizontal
            stack.spacing = 10
            views.append(stack)
            rows.append(row)
        }
        let stack = NSStackView(views: views)
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
            widthAnchor.constraint(equalToConstant: 340),
        ])
        section.position = (0.5, 0.5, 0.5)
        show(section)
    }

    /// True when any axis is cut.
    var isCutting: Bool { section.enabled.0 || section.enabled.1 || section.enabled.2 }

    private func edit(_ change: (inout GeoSection) -> Void) {
        var next = section
        change(&next)
        show(next)
        onChange?(next)
    }

    /// Shows `newSection` without reporting it.
    func show(_ newSection: GeoSection) {
        section = newSection
        for (axis, row) in rows.enumerated() {
            row.toggle.selected = section.isEnabled(axis) ? 1 : 0
            row.position.value = Double(section.positionOf(axis))
            row.side.selected = section.isFlipped(axis) ? 1 : 0
            row.position.isEnabled = section.isEnabled(axis)
        }
    }
}

extension GeoSection {
    // The C struct holds fixed arrays, which Swift sees as tuples.
    func isEnabled(_ axis: Int) -> Bool { [enabled.0, enabled.1, enabled.2][axis] }
    func positionOf(_ axis: Int) -> Float { [position.0, position.1, position.2][axis] }
    func isFlipped(_ axis: Int) -> Bool { [flip.0, flip.1, flip.2][axis] }

    mutating func setEnabled(_ axis: Int, _ value: Bool) {
        switch axis {
        case 0: enabled.0 = value
        case 1: enabled.1 = value
        default: enabled.2 = value
        }
    }

    mutating func setPosition(_ axis: Int, _ value: Float) {
        switch axis {
        case 0: position.0 = value
        case 1: position.1 = value
        default: position.2 = value
        }
    }

    mutating func setFlip(_ axis: Int, _ value: Bool) {
        switch axis {
        case 0: flip.0 = value
        case 1: flip.1 = value
        default: flip.2 = value
        }
    }
}
