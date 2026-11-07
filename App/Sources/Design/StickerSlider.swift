import AppKit

/// A value from 0 to 1 along an ink line, with a round paper thumb. Drawn with layers, like the
/// rest of the chrome. Arrow keys nudge it when it has focus.
@MainActor
final class StickerSlider: NSView {
    var value: Double = 0.5 {
        didSet { needsLayout = true }
    }
    var onChange: ((Double) -> Void)?
    var isEnabled = true {
        didSet { alphaValue = isEnabled ? 1 : 0.4 }
    }

    private let track = CALayer()
    private let fill = CALayer()
    private let thumb = CALayer()
    private static let thumbSize: CGFloat = 16

    init(help: String) {
        super.init(frame: .zero)
        wantsLayer = true
        track.backgroundColor = Palette.rule.cgColor
        track.cornerRadius = 1.5
        fill.backgroundColor = Palette.ink.cgColor
        fill.cornerRadius = 1.5
        thumb.backgroundColor = Palette.sheet.cgColor
        thumb.borderColor = Palette.ink.cgColor
        thumb.borderWidth = Metrics.line
        thumb.cornerRadius = Self.thumbSize / 2
        for sublayer in [track, fill, thumb] {
            layer?.addSublayer(sublayer)
        }
        toolTip = help
        setAccessibilityRole(.slider)
        setAccessibilityLabel(help)
        heightAnchor.constraint(equalToConstant: PillButton.height).isActive = true
        widthAnchor.constraint(greaterThanOrEqualToConstant: 120).isActive = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("StickerSlider is not used from Interface Builder")
    }

    override var acceptsFirstResponder: Bool { isEnabled }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let inset = Self.thumbSize / 2
        let width = max(bounds.width - inset * 2, 1)
        let mid = bounds.midY
        track.frame = CGRect(x: inset, y: mid - 1.5, width: width, height: 3)
        fill.frame = CGRect(x: inset, y: mid - 1.5, width: width * value, height: 3)
        thumb.frame = CGRect(
            x: inset + width * value - inset,
            y: mid - inset,
            width: Self.thumbSize,
            height: Self.thumbSize
        )
        CATransaction.commit()
        setAccessibilityValue(value)
    }

    override func mouseDown(with event: NSEvent) {
        track(event)
    }

    override func mouseDragged(with event: NSEvent) {
        track(event)
    }

    override func keyDown(with event: NSEvent) {
        switch event.specialKey {
        case .leftArrow?: set(value - 0.01)
        case .rightArrow?: set(value + 0.01)
        default: super.keyDown(with: event)
        }
    }

    private func track(_ event: NSEvent) {
        guard isEnabled else {
            return
        }
        let point = convert(event.locationInWindow, from: nil)
        let inset = Self.thumbSize / 2
        set((point.x - inset) / max(bounds.width - inset * 2, 1))
    }

    private func set(_ newValue: Double) {
        let clamped = min(max(newValue, 0), 1)
        guard clamped != value else {
            return
        }
        value = clamped
        onChange?(clamped)
    }
}
