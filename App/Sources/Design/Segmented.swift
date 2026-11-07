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

    /// `tips`, when given, explain each option on its own, in place of the pill's `help`.
    init(_ options: [String], selected: Int, help: String, tips: [String]? = nil) {
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
            let segment = Segment(title: option, divider: index > 0)
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
        private let fill = CALayer()
        private let underline = CALayer()
        private let divider = CALayer()

        init(title: String, divider hasDivider: Bool) {
            label = NSTextField(labelWithString: title)
            super.init(frame: .zero)
            wantsLayer = true
            layer?.addSublayer(fill)
            layer?.addSublayer(underline)
            if hasDivider {
                divider.backgroundColor = Palette.ink.cgColor
                layer?.addSublayer(divider)
            }
            underline.backgroundColor = Palette.ink.cgColor
            label.translatesAutoresizingMaskIntoConstraints = false
            addSubview(label)
            NSLayoutConstraint.activate([
                label.centerYAnchor.constraint(equalTo: centerYAnchor),
                label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 11),
                label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -11),
            ])
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
