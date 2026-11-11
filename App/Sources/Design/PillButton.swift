import AppKit

/// The interface's button: an ink-bordered pill that turns cream under the pointer, sinks a point
/// when pressed, and fills with ink when on. Drawn with layers, so nothing in it touches the 3D view.
@MainActor
final class PillButton: ChromeControl {
    enum Style {
        case normal
        case ghostly  // no border or fill until the pointer is over it
    }

    var isOn = false {
        didSet { stateChanged() }
    }

    private let style: Style
    private let background = CALayer()
    private let label = NSTextField(labelWithString: "")

    /// The button's text, for buttons made with one.
    var title: String {
        get { label.stringValue }
        set {
            label.stringValue = newValue
            invalidateIntrinsicContentSize()
        }
    }
    /// Folds the title away, leaving the icon, where a toolbar is short of room.
    var titleHidden = false {
        didSet {
            label.isHidden = titleHidden
            invalidateIntrinsicContentSize()
        }
    }
    private let icon = NSImageView()
    private let iconOnly: Bool

    static let height: CGFloat = 30

    init(title: String? = nil, icon name: Icons.Name? = nil, style: Style = .normal, help: String) {
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
        if let name {
            icon.image = Icons.image(name, size: 15)
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
        stateChanged()
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

    override func stateChanged() {
        let fill: NSColor
        let ink: NSColor
        if isOn {
            fill = Palette.ink
            ink = Palette.paper
        } else if isHovering && isEnabled {
            fill = Palette.cream
            ink = Palette.ink
        } else {
            fill = style == .ghostly ? .clear : Palette.sheet
            ink = Palette.ink
        }
        let bordered = style == .normal || isHovering || isOn
        Motion.animate {
            background.backgroundColor = fill.cgColor
            background.borderColor = bordered ? Palette.ink.cgColor : NSColor.clear.cgColor
            let offset: CGFloat = isPressing ? 1 : 0
            layer?.sublayerTransform = CATransform3DMakeTranslation(offset, -offset, 0)
        }
        label.textColor = ink
        icon.contentTintColor = ink
        alphaValue = isEnabled ? 1 : 0.4
    }
}
