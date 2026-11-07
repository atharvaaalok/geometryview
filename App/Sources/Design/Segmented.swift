import AppKit

/// A row of exclusive options in one pill. The chosen one is cream with an ink underline, since
/// blue is kept for what is selected in the model.
@MainActor
final class SegmentedPill: NSView {
    var onChange: ((Int) -> Void)?
    var selected: Int {
        didSet { refresh() }
    }

    private let frameLayer = CALayer()
    private let borderLayer = CALayer()  // above the segments, so fills never cover it
    private var segments: [Segment] = []

    init(_ options: [String], selected: Int, help: String) {
        self.selected = selected
        super.init(frame: .zero)
        wantsLayer = true
        frameLayer.cornerRadius = PillButton.height / 2
        frameLayer.masksToBounds = true
        frameLayer.backgroundColor = Palette.sheet.cgColor
        layer?.addSublayer(frameLayer)
        layer?.cornerRadius = PillButton.height / 2
        layer?.masksToBounds = true
        borderLayer.cornerRadius = PillButton.height / 2
        borderLayer.borderColor = Palette.ink.cgColor
        borderLayer.zPosition = 10
        setAccessibilityRole(.radioGroup)
        setAccessibilityLabel(help)
        toolTip = help

        var previous: NSView?
        for (index, option) in options.enumerated() {
            let segment = Segment(title: option, divider: index > 0)
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
        layer?.addSublayer(borderLayer)
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
        let scale = window?.backingScaleFactor ?? 2
        borderLayer.borderWidth = (Metrics.line * scale).rounded(.up) / scale
        frameLayer.frame = bounds
        borderLayer.frame = bounds
        CATransaction.commit()
    }

    private func refresh() {
        for (index, segment) in segments.enumerated() {
            segment.isChosen = index == selected
        }
    }

    @MainActor
    private final class Segment: NSView {
        var onPress: (() -> Void)?
        var isChosen = false {
            didSet { refresh() }
        }
        private let label: NSTextField
        private let fill = CALayer()
        private let underline = CALayer()
        private let divider = CALayer()
        private var hovering = false
        private var tracking: NSTrackingArea?

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
            refresh()
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
            underline.frame = CGRect(x: 0, y: 0, width: bounds.width, height: 2)
            divider.frame = CGRect(x: 0, y: 0, width: Metrics.line, height: bounds.height)
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
            refresh()
        }

        override func mouseUp(with event: NSEvent) {
            if bounds.contains(convert(event.locationInWindow, from: nil)) {
                onPress?()
            }
        }

        override func accessibilityPerformPress() -> Bool {
            onPress?()
            return true
        }

        override func isAccessibilitySelected() -> Bool { isChosen }

        private func refresh() {
            Motion.animate {
                fill.backgroundColor =
                    (isChosen || hovering) ? Palette.cream.cgColor : NSColor.clear.cgColor
                underline.opacity = isChosen ? 1 : 0
            }
            label.font = Typography.font(Typography.textSmall, isChosen ? .semibold : .medium)
            label.textColor = Palette.ink
        }
    }
}
