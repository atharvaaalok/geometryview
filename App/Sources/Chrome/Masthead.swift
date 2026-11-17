import AppKit

/// The title over the top left of the view: what is open, with a button that closes it, a line
/// about it, and import progress. Back and Forward stand before it, once there is somewhere to go.
/// It sits on the figure itself, not in a card, so text gets a paper halo to stay readable. Like a
/// window's title, dragging it moves the window.
@MainActor
final class Masthead: NSView {
    private let title = NSTextField(labelWithString: "")
    private let close = PillButton(icon: .close, style: .ghostly, help: "Close (⌘W)")
    private let back = PillButton(icon: .back, style: .ghostly, help: "Back (⌘[)")
    private let forward = PillButton(icon: .forward, style: .ghostly, help: "Forward (⌘])")
    private let arrows = NSStackView()
    private let subtitle = NSTextField(labelWithString: "")
    private let progress = ProgressPill()
    private let stack = NSStackView()
    private var textBesideArrows: NSLayoutConstraint?
    private var textAtEdge: NSLayoutConstraint?

    /// Closes what is open.
    var onClose: (() -> Void)? {
        get { close.onPress }
        set { close.onPress = newValue }
    }
    var onBack: (() -> Void)? {
        get { back.onPress }
        set { back.onPress = newValue }
    }
    var onForward: (() -> Void)? {
        get { forward.onPress }
        set { forward.onPress = newValue }
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        title.font = Typography.font(Typography.textLarge, .semibold)
        title.textColor = Palette.ink
        subtitle.font = Typography.font(Typography.textSmall)
        subtitle.textColor = Palette.muted
        for label in [title, subtitle] {
            label.lineBreakMode = .byTruncatingTail
            label.wantsLayer = true
            label.layer?.shadowColor = Palette.paper.cgColor
            label.layer?.shadowOpacity = 1
            label.layer?.shadowRadius = 3
            label.layer?.shadowOffset = .zero
        }
        ChromeLayout.name(title, "masthead.title")
        ChromeLayout.name(subtitle, "masthead.subtitle")
        ChromeLayout.name(progress, "masthead.progress")
        ChromeLayout.name(close, "masthead.close")
        ChromeLayout.name(back, "masthead.back")
        ChromeLayout.name(forward, "masthead.forward")
        let titleRow = NSStackView(views: [title, close])
        titleRow.orientation = .horizontal
        titleRow.alignment = .centerY
        titleRow.spacing = 4
        stack.setViews([titleRow, subtitle, progress], in: .leading)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 2
        stack.setCustomSpacing(10, after: subtitle)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        arrows.setViews([back, forward], in: .leading)
        arrows.orientation = .horizontal
        arrows.spacing = 0
        arrows.translatesAutoresizingMaskIntoConstraints = false
        addSubview(arrows)
        let beside = stack.leadingAnchor.constraint(equalTo: arrows.trailingAnchor, constant: 6)
        let edge = stack.leadingAnchor.constraint(equalTo: leadingAnchor)
        textBesideArrows = beside
        textAtEdge = edge
        NSLayoutConstraint.activate([
            arrows.leadingAnchor.constraint(equalTo: leadingAnchor),
            arrows.centerYAnchor.constraint(equalTo: titleRow.centerYAnchor),
            edge,
            stack.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        arrows.isHidden = true
        progress.isHidden = true
        isHidden = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("Masthead is not used from Interface Builder")
    }

    // Only the text itself, whose labels take no clicks of their own, and the buttons.
    override func hitTest(_ point: NSPoint) -> NSView? {
        let local = convert(point, from: superview)
        guard !isHidden,
            (!stack.isHidden && stack.frame.contains(local))
                || (!arrows.isHidden && arrows.frame.contains(local))
        else {
            return nil
        }
        if let hit = super.hitTest(point),
            [close, back, forward].contains(where: { hit.isDescendant(of: $0) })
        {
            return hit
        }
        return self
    }

    override func mouseDown(with event: NSEvent) {
        WindowDragging.handle(event, in: window)
    }

    /// Back and Forward: shown once there has been somewhere to go, each dimmed with nowhere its way.
    func setNavigation(shown: Bool, back canGoBack: Bool, forward canGoForward: Bool) {
        arrows.isHidden = !shown
        back.isEnabled = canGoBack
        forward.isEnabled = canGoForward
        textAtEdge?.isActive = !shown
        textBesideArrows?.isActive = shown
    }

    /// Nothing shows, but Back and Forward still lead somewhere: the arrows alone.
    func showArrowsOnly() {
        stack.isHidden = true
        isHidden = arrows.isHidden
    }

    /// On a dark sheet, Blueprint's or Studio's, the masthead, drawn straight on it, turns light.
    func setOnDarkSheet(_ dark: Bool) {
        title.textColor = dark ? Palette.inkOnDark : Palette.ink
        subtitle.textColor = dark ? Palette.mutedOnDark : Palette.muted
    }

    func show(title text: String, subtitle detail: String) {
        title.stringValue = text
        subtitle.stringValue = detail
        stack.isHidden = false
        isHidden = false
    }

    /// Shows progress as a fraction, or hides the bar when nil.
    func setProgress(_ fraction: Double?) {
        progress.isHidden = fraction == nil
        if let fraction {
            progress.fraction = fraction
        }
    }
}

/// A mustard bar in an ink pill.
@MainActor
final class ProgressPill: NSView {
    var fraction: Double = 0 {
        didSet { needsLayout = true }
    }
    private let fill = CALayer()
    private let edge = CALayer()

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = Palette.sheet.cgColor
        layer?.borderColor = Palette.ink.cgColor
        layer?.borderWidth = Metrics.line
        layer?.cornerRadius = 3
        layer?.masksToBounds = true
        fill.backgroundColor = Palette.mustard.cgColor
        edge.backgroundColor = Palette.ink.cgColor
        layer?.addSublayer(fill)
        layer?.addSublayer(edge)
        NSLayoutConstraint.activate([
            widthAnchor.constraint(equalToConstant: 240),
            heightAnchor.constraint(equalToConstant: 6),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("ProgressPill is not used from Interface Builder")
    }

    override func layout() {
        super.layout()
        let width = bounds.width * CGFloat(min(max(fraction, 0), 1))
        Motion.animate(Motion.slow) {
            fill.frame = CGRect(x: 0, y: 0, width: width, height: bounds.height)
            edge.frame = CGRect(
                x: width - Metrics.line,
                y: 0,
                width: Metrics.line,
                height: bounds.height
            )
        }
    }
}
