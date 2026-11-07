import AppKit

/// A small label card with a hard two-point shadow: the hover label, the hold hint.
@MainActor
final class Chip: NSView {
    private let shadowLayer = CALayer()
    private let sheet = CALayer()
    private let stack = NSStackView()

    var fillColor: NSColor = Palette.sheet {
        didSet { sheet.backgroundColor = fillColor.cgColor }
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.masksToBounds = false
        shadowLayer.backgroundColor = Palette.ink.cgColor
        shadowLayer.cornerRadius = Metrics.radiusChip
        sheet.backgroundColor = fillColor.cgColor
        sheet.borderColor = Palette.ink.cgColor
        sheet.cornerRadius = Metrics.radiusChip
        layer?.addSublayer(shadowLayer)
        layer?.addSublayer(sheet)
        stack.orientation = .horizontal
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 5),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -5),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("Chip is not used from Interface Builder")
    }

    /// Clicks pass through to whatever is below.
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    func setContent(_ views: [NSView]) {
        for view in stack.arrangedSubviews {
            stack.removeArrangedSubview(view)
            view.removeFromSuperview()
        }
        for view in views {
            stack.addArrangedSubview(view)
        }
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let scale = window?.backingScaleFactor ?? 2
        sheet.borderWidth = (Metrics.line * scale).rounded(.up) / scale
        sheet.frame = bounds
        shadowLayer.frame = bounds.offsetBy(dx: Metrics.shadowChip, dy: -Metrics.shadowChip)
        CATransaction.commit()
    }

    static func text(
        _ string: String,
        color: NSColor = Palette.ink,
        weight: Typography.Weight =
            .regular
    ) -> NSTextField {
        let label = NSTextField(labelWithString: string)
        label.font = Typography.font(Typography.textSmall, weight)
        label.textColor = color
        label.lineBreakMode = .byTruncatingTail
        return label
    }
}

/// A key on the keyboard, as in a shortcut.
@MainActor
final class KeyCap: NSView {
    init(_ key: String) {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = Palette.paper.cgColor
        layer?.borderColor = Palette.ink.cgColor
        layer?.borderWidth = 1.25
        layer?.cornerRadius = 5
        layer?.shadowColor = Palette.ink.cgColor
        layer?.shadowOpacity = 1
        layer?.shadowRadius = 0
        layer?.shadowOffset = CGSize(width: 1, height: -1)
        let label = NSTextField(labelWithString: key)
        label.font = Typography.font(Typography.textExtraSmall, .semibold)
        label.textColor = Palette.ink
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        NSLayoutConstraint.activate([
            heightAnchor.constraint(equalToConstant: 20),
            widthAnchor.constraint(greaterThanOrEqualToConstant: 20),
            widthAnchor.constraint(greaterThanOrEqualTo: label.widthAnchor, constant: 10),
            label.centerXAnchor.constraint(equalTo: centerXAnchor),
            label.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("KeyCap is not used from Interface Builder")
    }
}
