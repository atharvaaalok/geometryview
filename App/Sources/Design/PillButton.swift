import AppKit

/// The interface's button: an ink-bordered pill that turns cream under the pointer, sinks a point
/// when pressed, and fills with ink when on. Drawn with layers, so nothing in it touches the 3D view.
@MainActor
final class PillButton: NSView {
    enum Style {
        case normal
        case ghostly  // no border or fill until the pointer is over it
    }

    var onPress: (() -> Void)?
    var isOn = false {
        didSet { refresh() }
    }
    var isEnabled = true {
        didSet { refresh() }
    }

    private let style: Style
    private let background = CALayer()
    private let label = NSTextField(labelWithString: "")
    private let icon = NSImageView()
    private var hovering = false
    private var pressing = false
    private var tracking: NSTrackingArea?
    private let iconOnly: Bool

    static let height: CGFloat = 30

    init(title: String? = nil, symbol: String? = nil, style: Style = .normal, help: String) {
        self.style = style
        iconOnly = title == nil
        super.init(frame: .zero)
        wantsLayer = true
        layer?.masksToBounds = false
        background.cornerRadius = Self.height / 2
        background.borderColor = Palette.ink.cgColor
        layer?.addSublayer(background)
        toolTip = help
        setAccessibilityRole(.button)
        setAccessibilityLabel(help)

        var views: [NSView] = []
        if let symbol {
            let configuration = NSImage.SymbolConfiguration(pointSize: 12, weight: .medium)
            icon.image = NSImage(systemSymbolName: symbol, accessibilityDescription: help)?
                .withSymbolConfiguration(configuration)
            icon.contentTintColor = Palette.ink
            views.append(icon)
        }
        if let title {
            label.stringValue = title
            label.font = Typography.font(Typography.textSmall, .medium)
            label.textColor = Palette.ink
            views.append(label)
        }
        let stack = NSStackView(views: views)
        stack.orientation = .horizontal
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        // A button hugs its content, but may stretch when a layout asks it to fill a row.
        let natural =
            iconOnly
            ? widthAnchor.constraint(equalToConstant: Self.height)
            : widthAnchor.constraint(equalTo: stack.widthAnchor, constant: 24)
        natural.priority = .defaultLow
        NSLayoutConstraint.activate([
            heightAnchor.constraint(equalToConstant: Self.height),
            stack.centerXAnchor.constraint(equalTo: centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: centerYAnchor),
            widthAnchor.constraint(
                greaterThanOrEqualTo: stack.widthAnchor,
                constant: iconOnly ? 12 : 24
            ),
            iconOnly
                ? widthAnchor.constraint(greaterThanOrEqualToConstant: Self.height)
                : widthAnchor.constraint(greaterThanOrEqualToConstant: Self.height),
            natural,
        ])
        setContentHuggingPriority(.defaultHigh, for: .horizontal)
        refresh()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("PillButton is not used from Interface Builder")
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let scale = window?.backingScaleFactor ?? 2
        background.borderWidth = (Metrics.line * scale).rounded(.up) / scale
        background.frame = bounds
        CATransaction.commit()
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking {
            removeTrackingArea(tracking)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
            owner: self
        )
        addTrackingArea(area)
        tracking = area
    }

    override func mouseEntered(with event: NSEvent) {
        hovering = true
        refresh()
    }

    override func mouseExited(with event: NSEvent) {
        hovering = false
        pressing = false
        refresh()
    }

    override func mouseDown(with event: NSEvent) {
        guard isEnabled else {
            return
        }
        pressing = true
        refresh()
    }

    override func mouseUp(with event: NSEvent) {
        let inside = bounds.contains(convert(event.locationInWindow, from: nil))
        pressing = false
        refresh()
        if isEnabled, inside {
            onPress?()
        }
    }

    override func accessibilityPerformPress() -> Bool {
        onPress?()
        return true
    }

    private func refresh() {
        let fill: NSColor
        let ink: NSColor
        if isOn {
            fill = Palette.ink
            ink = Palette.paper
        } else if hovering && isEnabled {
            fill = Palette.cream
            ink = Palette.ink
        } else {
            fill = style == .ghostly ? .clear : Palette.sheet
            ink = Palette.ink
        }
        let bordered = style == .normal || hovering || isOn
        Motion.animate {
            background.backgroundColor = fill.cgColor
            background.borderColor = bordered ? Palette.ink.cgColor : NSColor.clear.cgColor
            let offset: CGFloat = pressing ? 1 : 0
            layer?.sublayerTransform = CATransform3DMakeTranslation(offset, -offset, 0)
        }
        label.textColor = ink
        icon.contentTintColor = ink
        alphaValue = isEnabled ? 1 : 0.4
    }
}
