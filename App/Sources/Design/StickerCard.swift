import AppKit

/// A card in the printed-sticker style: an opaque sheet with an ink border and a hard offset shadow.
///
/// The shadow is a second rounded rectangle rather than a blurred layer shadow, so it needs no
/// offscreen pass, and the opaque sheet needs no blending against the viewport.
@MainActor
class StickerCard: NSView {
    private let shadowLayer = CALayer()
    private let sheetLayer = CALayer()
    private let shadowOffset: CGFloat

    /// A card, or with the chip's radius and shadow, a smaller thing such as a menu.
    init(radius: CGFloat = Metrics.radiusCard, shadow: CGFloat = Metrics.shadowCard) {
        shadowOffset = shadow
        super.init(frame: .zero)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        layer?.masksToBounds = false
        shadowLayer.backgroundColor = Palette.ink.cgColor
        shadowLayer.cornerRadius = radius
        sheetLayer.backgroundColor = Palette.sheet.cgColor
        sheetLayer.borderColor = Palette.ink.cgColor
        sheetLayer.cornerRadius = radius
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
        shadowLayer.frame = bounds.offsetBy(dx: shadowOffset, dy: -shadowOffset)
        CATransaction.commit()
    }

    // MARK: Input stops here

    // Whatever reaches the card unhandled goes no further. Clicks on a card's labels used to reach
    // the 3D view, which then held a press it never saw released.
    override func mouseDown(with event: NSEvent) {}
    override func mouseDragged(with event: NSEvent) {}
    override func mouseUp(with event: NSEvent) {}
    override func rightMouseDown(with event: NSEvent) {}
    override func rightMouseDragged(with event: NSEvent) {}
    override func rightMouseUp(with event: NSEvent) {}
    override func otherMouseDown(with event: NSEvent) {}
    override func otherMouseDragged(with event: NSEvent) {}
    override func otherMouseUp(with event: NSEvent) {}
    override func scrollWheel(with event: NSEvent) {}
    override func magnify(with event: NSEvent) {}
}

/// The card over an empty viewport: an invitation at first, then progress or a problem.
@MainActor
final class StatusCard: StickerCard {
    private let heading = NSTextField(labelWithString: "")
    private let body = NSTextField(wrappingLabelWithString: "")
    private let openFiles = PillButton(title: "Open files", icon: .plus, help: "Open STEP or STL files (⌘O)")
    private let connectFolder = PillButton(
        title: "Connect a folder",
        icon: .folderPlus,
        help: "Connect a folder (⌥⌘O)"
    )
    private let buttons = NSStackView()
    private var loadingName = ""
    var onOpen: (() -> Void)?
    var onConnect: (() -> Void)?

    init() {
        super.init()
        heading.font = Typography.font(Typography.textHeading, .semibold)
        heading.textColor = Palette.ink
        body.font = Typography.font(Typography.textMedium)
        body.textColor = Palette.muted

        ChromeLayout.name(heading, "status.heading")
        ChromeLayout.name(body, "status.body")
        openFiles.onPress = { [weak self] in self?.onOpen?() }
        connectFolder.onPress = { [weak self] in self?.onConnect?() }
        buttons.setViews([openFiles, connectFolder], in: .leading)
        buttons.spacing = 8
        let stack = NSStackView(views: [heading, body, buttons])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        stack.setCustomSpacing(12, after: body)
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
        show("Nothing open yet", "Drop STEP or STL files to open them, or a folder to connect it.", offerWaysIn: true)
    }

    func showLoading(_ name: String) {
        loadingName = name
        show("Opening \(name)", "This only takes a moment.")
    }

    /// Progress is in units only the ratio of which matters.
    func showProgress(done: UInt32, total: UInt32) {
        guard total > 0, !isHidden else {
            return
        }
        let percent = Int((Double(done) / Double(total) * 100).rounded())
        show("Opening \(loadingName)", "\(percent)% of its surfaces are in.")
    }

    func showFailure(_ message: String) {
        show("That did not open", message, offerWaysIn: true)
    }

    private func show(_ title: String, _ detail: String, offerWaysIn: Bool = false) {
        heading.stringValue = title
        body.stringValue = detail
        buttons.isHidden = !offerWaysIn
        isHidden = false
    }
}
