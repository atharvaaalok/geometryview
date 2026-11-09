import AppKit
import GeometryViewEngine

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
/// name, and at the end a count, a mark for what is hidden or ghosted, or, under the pointer,
/// buttons to ghost, hide and isolate.
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

    enum Action {
        case ghost, hide, isolate
    }

    /// Called with a row button's action.
    var onAction: ((Action) -> Void)?
    /// Shows the row buttons in place of the count, while the pointer is on the row.
    var showsActions = false {
        didSet {
            if showsActions != oldValue {
                refreshEnd()
            }
        }
    }

    private let chevron = NSImageView()
    private let glyph = NSImageView()
    private let swatch = SwatchView()
    private let label = NSTextField(labelWithString: "")
    private let count = NSTextField(labelWithString: "")
    private let stateMark = NSImageView()
    private let ghostButton = RowButton(icon: .ghost)
    private let hideButton = RowButton(icon: .eye)
    private let isolateButton = RowButton(icon: .scope)
    private let actions = NSStackView()
    private var labelBeforeCount: NSLayoutConstraint?
    private var labelBeforeActions: NSLayoutConstraint?
    private var indent: NSLayoutConstraint?
    private var name = ""
    private var state: GeoNodeState?
    private static let indentStep: CGFloat = 14
    private static let chevronSize: CGFloat = 16

    init() {
        super.init(frame: .zero)
        identifier = Self.identifier
        chevron.contentTintColor = Palette.ink
        glyph.contentTintColor = Palette.ink
        stateMark.contentTintColor = Palette.muted
        label.font = Typography.font(Typography.textMedium)
        label.textColor = Palette.ink
        label.lineBreakMode = .byTruncatingTail
        label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        count.font = Typography.font(Typography.textSmall)
        count.textColor = Palette.muted
        count.alignment = .right
        for view in [count, stateMark, actions] as [NSView] {
            view.setContentCompressionResistancePriority(.required, for: .horizontal)
            view.setContentHuggingPriority(.required, for: .horizontal)
        }
        actions.setViews([ghostButton, hideButton, isolateButton], in: .leading)
        actions.spacing = 1
        ghostButton.onPress = { [weak self] in self?.onAction?(.ghost) }
        hideButton.onPress = { [weak self] in self?.onAction?(.hide) }
        isolateButton.onPress = { [weak self] in self?.onAction?(.isolate) }
        isolateButton.toolTip = "Show only this, framed"

        for (view, name) in [(chevron, "chevron"), (glyph, "glyph"), (swatch, "swatch"), (label, "name"), (count, "count")] as [(NSView, String)] {
            ChromeLayout.name(view, "tree.\(name)")
        }
        for view in [chevron, glyph, swatch, label, count, stateMark, actions] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            addSubview(view)
        }
        let indent = chevron.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8)
        self.indent = indent
        let beforeCount = label.trailingAnchor.constraint(
            lessThanOrEqualTo: count.leadingAnchor,
            constant: -8
        )
        let beforeActions = label.trailingAnchor.constraint(
            lessThanOrEqualTo: actions.leadingAnchor,
            constant: -4
        )
        labelBeforeCount = beforeCount
        labelBeforeActions = beforeActions
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
            beforeCount,
            count.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            count.centerYAnchor.constraint(equalTo: centerYAnchor),
            stateMark.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            stateMark.centerYAnchor.constraint(equalTo: centerYAnchor),
            actions.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            actions.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("TreeCell is not used from Interface Builder")
    }

    /// `state` is nil for a file's row, which has no buttons.
    func configure(
        name: String,
        depth: Int,
        mark: Mark,
        isDocument: Bool,
        disclosure: Disclosure,
        count text: String,
        state: GeoNodeState?
    ) {
        indent?.constant = 8 + CGFloat(depth) * Self.indentStep
        self.name = name
        self.state = state
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
        refreshName()
        refreshEnd()
    }

    // A hidden row's name is faint and struck through, a ghosted one's muted.
    private func refreshName() {
        let color: NSColor
        var attributes: [NSAttributedString.Key: Any] = [:]
        switch state {
        case GeoNodeHidden:
            color = Palette.faint
            attributes[.strikethroughStyle] = NSUnderlineStyle.single.rawValue
            attributes[.strikethroughColor] = Palette.faint
        case GeoNodeGhosted:
            color = Palette.muted
        default:
            color = Palette.ink
        }
        attributes[.foregroundColor] = color
        attributes[.font] = label.font
        label.attributedStringValue = NSAttributedString(string: name, attributes: attributes)
    }

    // The row's end: buttons under the pointer, else a mark for what is hidden or ghosted, else
    // the count.
    private func refreshEnd() {
        let buttons = showsActions && state != nil
        actions.isHidden = !buttons
        labelBeforeActions?.isActive = buttons
        labelBeforeCount?.isActive = !buttons
        let hidden = state == GeoNodeHidden
        let ghosted = state == GeoNodeGhosted
        ghostButton.isOn = ghosted
        ghostButton.toolTip = ghosted ? "Unghost" : "Ghost"
        hideButton.isOn = hidden
        hideButton.icon = hidden ? .eyeSlash : .eye
        hideButton.toolTip = hidden ? "Show" : "Hide"

        let mark: NSImage? =
            switch state {
            case GeoNodeHidden: TreeGlyph.hidden
            case GeoNodeGhosted: TreeGlyph.ghosted
            case GeoNodeMixed: TreeGlyph.mixed
            default: nil
            }
        stateMark.image = mark
        stateMark.isHidden = buttons || mark == nil
        count.isHidden = buttons || mark != nil
    }

    func disclosureHit(_ point: NSPoint) -> Bool {
        !chevron.isHidden && chevron.frame.insetBy(dx: -4, dy: -6).contains(point)
    }
}

/// A small square icon button for tree rows: bare until the pointer is over it, then outlined, and
/// sunk while its state is on.
@MainActor
final class RowButton: ChromeControl {
    var isOn = false {
        didSet { stateChanged() }
    }
    var icon: Icons.Name {
        didSet {
            if icon != oldValue {
                iconView.image = Icons.image(icon, size: 13)
            }
        }
    }
    private let iconView = NSImageView()
    static let size: CGFloat = 22

    init(icon: Icons.Name) {
        self.icon = icon
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 6
        iconView.image = Icons.image(icon, size: 13)
        iconView.contentTintColor = Palette.ink
        iconView.translatesAutoresizingMaskIntoConstraints = false
        addSubview(iconView)
        NSLayoutConstraint.activate([
            widthAnchor.constraint(equalToConstant: Self.size),
            heightAnchor.constraint(equalToConstant: Self.size),
            iconView.centerXAnchor.constraint(equalTo: centerXAnchor),
            iconView.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
        setAccessibilityRole(.button)
        stateChanged()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("RowButton is not used from Interface Builder")
    }

    override var toolTip: String? {
        didSet { setAccessibilityLabel(toolTip) }
    }

    override func layout() {
        super.layout()
        let scale = window?.backingScaleFactor ?? 2
        layer?.borderWidth = (1.25 * scale).rounded(.up) / scale
    }

    override func stateChanged() {
        let outlined = isOn || isHovering
        let fill: NSColor = isOn ? Palette.sheetSunk : (isHovering ? Palette.sheet : .clear)
        Motion.animate {
            layer?.backgroundColor = fill.cgColor
            layer?.borderColor = outlined ? Palette.ink.cgColor : NSColor.clear.cgColor
        }
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

/// The tree's small drawings, from the shared icons, as template images so they take the ink colour.
@MainActor
enum TreeGlyph {
    /// Two linked hexagons: parts joined into an assembly.
    static let assembly = Icons.image(.assembly, size: 16)
    static let closed = Icons.image(.chevron, size: 12)
    static let open = Icons.image(.chevron, size: 12, turned: true)
    static let hidden = Icons.image(.eyeSlash, size: 12)
    static let ghosted = Icons.image(.ghost, size: 12)
    /// A circle, half filled: some of what is under the row is hidden or ghosted.
    static let mixed = Icons.image(.seeThrough, size: 12)
    static let document = Icons.image(.file, size: 14)
}
