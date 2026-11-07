import AppKit

/// The title over the top left of the view: what is open, a line about it, and import progress.
/// It sits on the figure itself, not in a card, so text gets a paper halo to stay readable.
@MainActor
final class Masthead: NSView {
    private let title = NSTextField(labelWithString: "")
    private let subtitle = NSTextField(labelWithString: "")
    private let progress = ProgressPill()

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
        let stack = NSStackView(views: [title, subtitle, progress])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 2
        stack.setCustomSpacing(10, after: subtitle)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        progress.isHidden = true
        isHidden = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("Masthead is not used from Interface Builder")
    }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    func show(title text: String, subtitle detail: String) {
        title.stringValue = text
        subtitle.stringValue = detail
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
