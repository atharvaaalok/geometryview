import AppKit

/// A card in the printed-sticker style: an opaque sheet with an ink border and a hard offset shadow.
///
/// The shadow is a second rounded rectangle rather than a blurred layer shadow, so it needs no
/// offscreen pass, and the opaque sheet needs no blending against the viewport.
@MainActor
final class StickerCard: NSView {
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

    /// The card shown when nothing is open.
    static func emptyState() -> StickerCard {
        let card = StickerCard()

        let heading = NSTextField(labelWithString: "Nothing open yet")
        heading.font = Typography.font(Typography.textHeading, .semibold)
        heading.textColor = Palette.ink

        let body = NSTextField(
            wrappingLabelWithString: "Drop a STEP or STL file here, or press ⌘O to choose one.")
        body.font = Typography.font(Typography.textMedium)
        body.textColor = Palette.muted

        let stack = NSStackView(views: [heading, body])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        card.addSubview(stack)

        let inset = Metrics.gap + 4
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: inset),
            stack.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -inset),
            stack.topAnchor.constraint(equalTo: card.topAnchor, constant: inset),
            stack.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -inset),
        ])
        return card
    }
}
