import AppKit

/// A card in the printed-sticker style: an opaque sheet with an ink border and a hard offset shadow.
///
/// The shadow is a second rounded rectangle rather than a blurred layer shadow, so it needs no
/// offscreen pass, and the opaque sheet needs no blending against the viewport.
@MainActor
class StickerCard: NSView {
    private let shadowLayer = CALayer()
    private let sheetLayer = CALayer()

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        layer?.masksToBounds = false
        shadowLayer.backgroundColor = Palette.ink.cgColor
        shadowLayer.cornerRadius = Metrics.radiusCard
        sheetLayer.backgroundColor = Palette.sheet.cgColor
        sheetLayer.borderColor = Palette.ink.cgColor
        sheetLayer.cornerRadius = Metrics.radiusCard
        layer?.addSublayer(shadowLayer)
        layer?.addSublayer(sheetLayer)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("StickerCard is not used from Interface Builder")
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        // Strokes snap to whole device pixels, so a 1.5 pt line stays crisp on a 1x display.
        let scale = window?.backingScaleFactor ?? 2
        sheetLayer.borderWidth = (Metrics.line * scale).rounded(.up) / scale
        sheetLayer.frame = bounds
        shadowLayer.frame = bounds.offsetBy(dx: Metrics.shadowCard, dy: -Metrics.shadowCard)
        CATransaction.commit()
    }

}

/// The card over an empty viewport: an invitation at first, then progress or a problem.
@MainActor
final class StatusCard: StickerCard {
    private let heading = NSTextField(labelWithString: "")
    private let body = NSTextField(wrappingLabelWithString: "")

    override init(frame: NSRect) {
        super.init(frame: frame)
        heading.font = Typography.font(Typography.textHeading, .semibold)
        heading.textColor = Palette.ink
        body.font = Typography.font(Typography.textMedium)
        body.textColor = Palette.muted

        let stack = NSStackView(views: [heading, body])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)

        let inset = Metrics.gap + 4
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: inset),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -inset),
        ])
        showInvitation()
    }

    func showInvitation() {
        show("Nothing open yet", "Drop a STEP or STL file here, or press ⌘O to choose one.")
    }

    func showLoading(_ name: String) {
        show("Opening \(name)", "This only takes a moment.")
    }

    func showFailure(_ message: String) {
        show("That did not open", message)
    }

    private func show(_ title: String, _ detail: String) {
        heading.stringValue = title
        body.stringValue = detail
        isHidden = false
    }
}
