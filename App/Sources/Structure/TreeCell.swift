import AppKit
import GeometryEngine

/// A tree row's background: cream while the pointer is over the row or over its part in the view,
/// and the cobalt wash with a bar down its left edge while selected.
@MainActor
final class TreeRowView: NSTableRowView {
    var isHovered = false {
        didSet {
            if isHovered != oldValue {
                needsDisplay = true
            }
        }
    }

    override var isEmphasized: Bool {
        get { false }
        set {}
    }

    private var plate: NSRect { bounds.insetBy(dx: 6, dy: 1) }

    override func drawBackground(in dirtyRect: NSRect) {
        guard isHovered, !isSelected else {
            return
        }
        Palette.cream.setFill()
        NSBezierPath(roundedRect: plate, xRadius: 6, yRadius: 6).fill()
    }

    override func drawSelection(in dirtyRect: NSRect) {
        let shape = NSBezierPath(roundedRect: plate, xRadius: 6, yRadius: 6)
        Palette.cobaltWash.setFill()
        shape.fill()
        NSGraphicsContext.saveGraphicsState()
        shape.addClip()
        Palette.cobaltMid.setFill()
        NSRect(x: plate.minX, y: plate.minY, width: 3, height: plate.height).fill()
        NSGraphicsContext.restoreGraphicsState()
    }
}

/// One tree row: a disclosure chevron, the assembly glyph or a swatch of the part's colour, the
/// name, and a count of what is inside.
@MainActor
final class TreeCell: NSTableCellView {
    static let identifier = NSUserInterfaceItemIdentifier("TreeCell")

    enum Disclosure {
        case none, open, closed
    }

    /// What stands before the name.
    enum Mark {
        case document
        case assembly
        case swatch(NSColor?, small: Bool)
    }

    private let chevron = NSImageView()
    private let glyph = NSImageView()
    private let swatch = SwatchView()
    private let label = NSTextField(labelWithString: "")
    private let count = NSTextField(labelWithString: "")
    private var indent: NSLayoutConstraint?
    private static let indentStep: CGFloat = 14
    private static let chevronSize: CGFloat = 16

    init() {
        super.init(frame: .zero)
        identifier = Self.identifier
        chevron.contentTintColor = Palette.ink
        glyph.contentTintColor = Palette.ink
        label.font = Typography.font(Typography.textMedium)
        label.textColor = Palette.ink
        label.lineBreakMode = .byTruncatingTail
        label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        count.font = Typography.font(Typography.textSmall)
        count.textColor = Palette.muted
        count.alignment = .right
        count.setContentCompressionResistancePriority(.required, for: .horizontal)
        count.setContentHuggingPriority(.required, for: .horizontal)

        for view in [chevron, glyph, swatch, label, count] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            addSubview(view)
        }
        let indent = chevron.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8)
        self.indent = indent
        NSLayoutConstraint.activate([
            indent,
            chevron.widthAnchor.constraint(equalToConstant: Self.chevronSize),
            chevron.heightAnchor.constraint(equalToConstant: Self.chevronSize),
            chevron.centerYAnchor.constraint(equalTo: centerYAnchor),
            glyph.leadingAnchor.constraint(equalTo: chevron.trailingAnchor, constant: 3),
            glyph.centerYAnchor.constraint(equalTo: centerYAnchor),
            glyph.widthAnchor.constraint(equalToConstant: 16),
            swatch.centerXAnchor.constraint(equalTo: glyph.centerXAnchor),
            swatch.centerYAnchor.constraint(equalTo: centerYAnchor),
            label.leadingAnchor.constraint(equalTo: glyph.trailingAnchor, constant: 7),
            label.centerYAnchor.constraint(equalTo: centerYAnchor),
            count.leadingAnchor.constraint(greaterThanOrEqualTo: label.trailingAnchor, constant: 8),
            count.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            count.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("TreeCell is not used from Interface Builder")
    }

    func configure(
        name: String,
        depth: Int,
        mark: Mark,
        isDocument: Bool,
        disclosure: Disclosure,
        count text: String
    ) {
        indent?.constant = 8 + CGFloat(depth) * Self.indentStep
        label.stringValue = name
        label.font = Typography.font(Typography.textMedium, isDocument ? .semibold : .regular)
        count.stringValue = text
        chevron.isHidden = disclosure == .none
        chevron.image = disclosure == .open ? TreeGlyph.open : TreeGlyph.closed
        switch mark {
        case .document:
            glyph.image = TreeGlyph.document
            glyph.isHidden = false
            swatch.isHidden = true
        case .assembly:
            glyph.image = TreeGlyph.assembly
            glyph.isHidden = false
            swatch.isHidden = true
        case .swatch(let color, let small):
            glyph.isHidden = true
            swatch.isHidden = false
            swatch.show(color, small: small)
        }
    }

    func disclosureHit(_ point: NSPoint) -> Bool {
        !chevron.isHidden && chevron.frame.insetBy(dx: -4, dy: -6).contains(point)
    }
}

/// A part's colour as a small ink-bordered tile, split on the diagonal into the colour and a
/// slightly darker shade of it, like a printed colour chip.
@MainActor
final class SwatchView: NSView {
    private let shade = CALayer()
    private let light = CAShapeLayer()
    private var size: NSLayoutConstraint?
    private var small = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.masksToBounds = true
        layer?.borderColor = Palette.ink.cgColor
        layer?.addSublayer(shade)
        layer?.addSublayer(light)
        let size = widthAnchor.constraint(equalToConstant: 12)
        self.size = size
        NSLayoutConstraint.activate([size, heightAnchor.constraint(equalTo: widthAnchor)])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("SwatchView is not used from Interface Builder")
    }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    /// Shows a colour, or a neutral tile while the colour is not yet known.
    func show(_ color: NSColor?, small: Bool) {
        self.small = small
        size?.constant = small ? 9 : 12
        let base = color ?? Palette.sheetSunk
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        light.fillColor = base.cgColor
        shade.backgroundColor = (base.blended(withFraction: 0.16, of: Palette.ink) ?? base).cgColor
        CATransaction.commit()
        needsLayout = true
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let scale = window?.backingScaleFactor ?? 2
        layer?.borderWidth = (1.25 * scale).rounded(.up) / scale
        layer?.cornerRadius = small ? 2 : 3
        shade.frame = bounds
        light.frame = bounds
        // The colour fills the top left, up to a line across the diagonal 55% of the way along.
        let s = bounds.width
        let path = CGMutablePath()
        path.move(to: CGPoint(x: 0, y: s))
        path.addLine(to: CGPoint(x: s, y: s))
        path.addLine(to: CGPoint(x: s, y: s - 0.1 * s))
        path.addLine(to: CGPoint(x: 0.1 * s, y: 0))
        path.addLine(to: CGPoint(x: 0, y: 0))
        path.closeSubpath()
        light.path = path
        CATransaction.commit()
    }
}

/// Small line drawings for the tree, as template images so they take the ink colour.
@MainActor
enum TreeGlyph {
    /// Two linked hexagons: parts joined into an assembly.
    static let assembly: NSImage = {
        let image = NSImage(size: NSSize(width: 16, height: 16), flipped: true) { _ in
            NSColor.black.setStroke()
            for (cx, cy) in [(5.6, 6.0), (10.4, 10.0)] {
                let path = NSBezierPath()
                for corner in 0..<6 {
                    let angle = Double(corner) * .pi / 3 + .pi / 6
                    let point = NSPoint(x: cx + 4.2 * cos(angle), y: cy + 4.2 * sin(angle))
                    corner == 0 ? path.move(to: point) : path.line(to: point)
                }
                path.close()
                path.lineWidth = 1.25
                path.lineJoinStyle = .round
                path.stroke()
            }
            return true
        }
        image.isTemplate = true
        return image
    }()

    static let closed = chevron("chevron.right")
    static let open = chevron("chevron.down")

    private static func chevron(_ name: String) -> NSImage {
        NSImage(systemSymbolName: name, accessibilityDescription: nil)?
            .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: 9, weight: .bold))
            ?? NSImage()
    }

    static let document: NSImage =
        NSImage(systemSymbolName: "doc", accessibilityDescription: "File")?
        .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: 11, weight: .medium))
        ?? NSImage()
}
