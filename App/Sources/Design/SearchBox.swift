import AppKit

/// The search field the web viewer draws, rather than the system's: a white rounded box whose line
/// darkens while it has the focus, the shared magnifier, the text, and at its end how many match.
/// Its owner is the text field's delegate, for each keystroke, Return and Escape.
@MainActor
final class SearchBox: NSView {
    let field = Field()
    let match = NSTextField(labelWithString: "")
    private let icon = NSImageView()
    static let height: CGFloat = 28

    /// The text field, telling its box when it takes the focus. Losing it reaches the box through
    /// `editingEnded`, since the field editor holds the focus while typing.
    final class Field: NSTextField {
        var onFocus: (() -> Void)?
        override func becomeFirstResponder() -> Bool {
            let taken = super.becomeFirstResponder()
            if taken {
                onFocus?()
            }
            return taken
        }
    }

    init(placeholder: String) {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = NSColor.white.cgColor
        layer?.cornerRadius = Self.height / 2
        layer?.borderColor = Palette.rule.cgColor
        icon.image = Icons.image(.search, size: 14)
        icon.contentTintColor = Palette.muted
        field.isBordered = false
        field.drawsBackground = false
        field.focusRingType = .none
        field.font = Typography.font(Typography.textMedium)
        field.textColor = Palette.ink
        field.cell?.isScrollable = true
        field.cell?.wraps = false
        field.placeholderAttributedString = NSAttributedString(
            string: placeholder,
            attributes: [.font: Typography.font(Typography.textMedium), .foregroundColor: Palette.muted]
        )
        field.onFocus = { [weak self] in self?.setFocused(true) }
        match.font = Typography.font(Typography.textExtraSmall)
        match.textColor = Palette.muted
        match.setContentCompressionResistancePriority(.required, for: .horizontal)
        match.setContentHuggingPriority(.required, for: .horizontal)
        for view in [icon, field, match] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            addSubview(view)
        }
        // As the page lays it out: 10 inside its line, the icon, 6, the text, 6, the count.
        let inside = 10 + Metrics.line
        NSLayoutConstraint.activate([
            heightAnchor.constraint(equalToConstant: Self.height),
            icon.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inside),
            icon.centerYAnchor.constraint(equalTo: centerYAnchor),
            icon.widthAnchor.constraint(equalToConstant: 14),
            icon.heightAnchor.constraint(equalToConstant: 14),
            field.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 6 - 2),
            field.centerYAnchor.constraint(equalTo: centerYAnchor),
            match.leadingAnchor.constraint(equalTo: field.trailingAnchor, constant: 6 - 4),
            match.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inside + 2),
            match.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("SearchBox is not used from Interface Builder")
    }

    /// Called by the owner when the field editor finishes.
    func editingEnded() {
        setFocused(false)
    }

    private func setFocused(_ focused: Bool) {
        layer?.borderColor = (focused ? Palette.ink : Palette.rule).cgColor
    }

    override func layout() {
        super.layout()
        layer?.borderWidth = Metrics.line
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(field)
    }
}
