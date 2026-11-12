import AppKit

/// A row of exclusive options in one pill. The chosen one is cream and semibold with an ink
/// underline, since blue is kept for what is selected in the model. Hover is a paler wash, so it
/// never reads as chosen.
@MainActor
final class SegmentedPill: NSView {
    var onChange: ((Int) -> Void)?
    var selected: Int {
        didSet { refresh() }
    }
    /// A disabled pill is greyed and ignores clicks, but keeps its place in the layout.
    var isEnabled = true {
        didSet {
            for segment in segments {
                segment.isEnabled = isEnabled
            }
            alphaValue = isEnabled ? 1 : 0.4
        }
    }

    private let frameLayer = CALayer()
    // A view added after the segments, so its border is drawn over their fills however the layers
    // are rendered.
    private let border = Outline()
    private var segments: [Segment] = []

    /// `tips`, when given, explain each option on its own, in place of the pill's `help`. `icons`
    /// and `keys`, when given, put an icon before each option and its key after it, as the
    /// gallery's view switch has them.
    init(
        _ options: [String],
        selected: Int,
        help: String,
        tips: [String]? = nil,
        icons: [Icons.Name]? = nil,
        keys: [String]? = nil
    ) {
        self.selected = selected
        super.init(frame: .zero)
        wantsLayer = true
        frameLayer.cornerRadius = PillButton.height / 2
        frameLayer.masksToBounds = true
        frameLayer.backgroundColor = Palette.sheet.cgColor
        layer?.addSublayer(frameLayer)
        layer?.cornerRadius = PillButton.height / 2
        layer?.masksToBounds = true
        setAccessibilityRole(.radioGroup)
        setAccessibilityLabel(help)
        toolTip = help

        var previous: NSView?
        for (index, option) in options.enumerated() {
            let segment = Segment(
                title: option,
                divider: index > 0,
                icon: icons?[index],
                key: keys?[index]
            )
            ChromeLayout.name(segment, "structure.segment")
            segment.toolTip = tips?[index]
            segment.onPress = { [weak self] in
                guard let self, self.selected != index else {
                    return
                }
                self.selected = index
                self.onChange?(index)
            }
            segment.translatesAutoresizingMaskIntoConstraints = false
            addSubview(segment)
            NSLayoutConstraint.activate([
                segment.topAnchor.constraint(equalTo: topAnchor),
                segment.bottomAnchor.constraint(equalTo: bottomAnchor),
                segment.leadingAnchor.constraint(
                    equalTo: previous?.trailingAnchor ?? leadingAnchor
                ),
            ])
            previous = segment
            segments.append(segment)
        }
        if let previous {
            previous.trailingAnchor.constraint(equalTo: trailingAnchor).isActive = true
        }
        border.translatesAutoresizingMaskIntoConstraints = false
        addSubview(border)
        NSLayoutConstraint.activate([
            border.leadingAnchor.constraint(equalTo: leadingAnchor),
            border.trailingAnchor.constraint(equalTo: trailingAnchor),
            border.topAnchor.constraint(equalTo: topAnchor),
            border.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        heightAnchor.constraint(equalToConstant: PillButton.height).isActive = true
        refresh()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("SegmentedPill is not used from Interface Builder")
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        frameLayer.frame = bounds
        CATransaction.commit()
    }

    private func refresh() {
        for (index, segment) in segments.enumerated() {
            segment.isChosen = index == selected
        }
    }

    @MainActor
    private final class Segment: ChromeControl {
        var isChosen = false {
            didSet { stateChanged() }
        }
        private let label: NSTextField
        private let keyLabel: NSTextField?
        private let fill = CALayer()
        private let underline = CALayer()
        private let divider = CALayer()

        init(title: String, divider hasDivider: Bool, icon: Icons.Name? = nil, key: String? = nil) {
            label = NSTextField(labelWithString: title)
            keyLabel = key.map { NSTextField(labelWithString: $0) }
            super.init(frame: .zero)
            wantsLayer = true
            layer?.addSublayer(fill)
            layer?.addSublayer(underline)
            if hasDivider {
                divider.backgroundColor = Palette.ink.cgColor
                layer?.addSublayer(divider)
            }
            underline.backgroundColor = Palette.ink.cgColor
            if icon == nil, keyLabel == nil {
                label.translatesAutoresizingMaskIntoConstraints = false
                addSubview(label)
                NSLayoutConstraint.activate([
                    label.centerYAnchor.constraint(equalTo: centerYAnchor),
                    label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 11),
                    label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -11),
                ])
            } else {
                // The icon, the name and its key on one baseline, 9 inside each end.
                var views: [NSView] = []
                if let icon {
                    let image = NSImageView(image: Icons.image(icon, size: 15))
                    image.contentTintColor = Palette.ink
                    views.append(image)
                }
                views.append(label)
                if let keyLabel {
                    keyLabel.font = Typography.font(9.5, .medium)
                    keyLabel.textColor = Palette.faint
                    views.append(keyLabel)
                }
                let row = NSStackView(views: views)
                row.orientation = .horizontal
                row.spacing = 6
                row.alignment = .centerY
                row.translatesAutoresizingMaskIntoConstraints = false
                addSubview(row)
                NSLayoutConstraint.activate([
                    row.centerYAnchor.constraint(equalTo: centerYAnchor),
                    row.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 9),
                    row.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -9),
                ])
            }
            setAccessibilityRole(.radioButton)
            setAccessibilityLabel(title)
            stateChanged()
        }

        @available(*, unavailable)
        required init?(coder: NSCoder) {
            fatalError("Segment is not used from Interface Builder")
        }

        override func layout() {
            super.layout()
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            fill.frame = bounds
            // Just above the pill's border, which is drawn over the segments.
            let scale = window?.backingScaleFactor ?? 2
            let border = (Metrics.line * scale).rounded(.up) / scale
            underline.frame = CGRect(x: 0, y: border, width: bounds.width, height: 2)
            divider.frame = CGRect(x: 0, y: 0, width: Metrics.line, height: bounds.height)
            CATransaction.commit()
        }

        override func isAccessibilitySelected() -> Bool { isChosen }

        override func stateChanged() {
            let color: NSColor
            if isChosen {
                color = Palette.cream
            } else if isHovering && isEnabled {
                color = Palette.creamWash
            } else {
                color = .clear
            }
            Motion.animate {
                fill.backgroundColor = color.cgColor
                underline.opacity = isChosen ? 1 : 0
            }
            label.font = Typography.font(Typography.textSmall, isChosen ? .semibold : .medium)
            label.textColor = Palette.ink
            keyLabel?.textColor = isChosen ? Palette.muted : Palette.faint
        }
    }

    /// The pill's ink outline. It takes no clicks.
    @MainActor
    private final class Outline: NSView {
        override init(frame: NSRect) {
            super.init(frame: frame)
            wantsLayer = true
            layer?.cornerRadius = PillButton.height / 2
            layer?.borderColor = Palette.ink.cgColor
        }

        @available(*, unavailable)
        required init?(coder: NSCoder) {
            fatalError("Outline is not used from Interface Builder")
        }

        override func hitTest(_ point: NSPoint) -> NSView? { nil }

        override func layout() {
            super.layout()
            let scale = window?.backingScaleFactor ?? 2
            layer?.borderWidth = (Metrics.line * scale).rounded(.up) / scale
        }
    }
}
