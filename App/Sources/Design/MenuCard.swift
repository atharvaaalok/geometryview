import AppKit

/// A menu as the page draws one, rather than the system's: a small sticker card of plain rows,
/// cream under the pointer. It opens above what it belongs to and closes when a row is chosen, on
/// Escape, or on a click anywhere else.
/// One line of a menu: a row to choose, with an icon, a check mark and whether it can be chosen
/// now; or a heading over the rows after it.
enum MenuItem {
    case row(title: String, icon: Icons.Name?, checked: Bool, enabled: Bool, action: () -> Void)
    case heading(String)
}

@MainActor
final class MenuCard: StickerCard {
    private var monitor: Any?
    private weak var anchor: NSView?
    private let stack = NSStackView()
    /// Rows have an icon column, for menus whose rows have icons.
    private let iconColumn: Bool

    /// A menu of plain rows, such as the standard views.
    convenience init(items: [(title: String, action: () -> Void)]) {
        self.init(iconColumn: false)
        setItems(items.map { .row(title: $0.title, icon: nil, checked: false, enabled: true, action: $0.action) })
    }

    init(iconColumn: Bool) {
        self.iconColumn = iconColumn
        super.init(radius: Metrics.radiusChip, shadow: Metrics.shadowChip)
        isHidden = true
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 0
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        // The page's padding of 6, inside the line.
        let inset = 6 + Metrics.line
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: inset),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -inset),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: inset),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -inset),
        ])
    }

    /// Replaces the menu's lines.
    func setItems(_ items: [MenuItem]) {
        stack.arrangedSubviews.forEach { $0.removeFromSuperview() }
        for item in items {
            let view: NSView
            switch item {
            case .row(let title, let icon, let checked, let enabled, let action):
                let row = MenuRow(title: title, icon: icon, iconColumn: iconColumn, checked: checked)
                row.isEnabled = enabled
                row.onPress = { [weak self] in
                    self?.close()
                    action()
                }
                view = row
            case .heading(let title):
                view = MenuHeading(title: title)
            }
            stack.addArrangedSubview(view)
            view.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
    }

    var isOpen: Bool { !isHidden }

    /// Opens 10 above `anchor`, kept 8 inside the window, or closes if open.
    func toggle(above anchor: NSView) {
        guard !isOpen, let container = superview else {
            close()
            return
        }
        self.anchor = anchor
        let size = fittingSize
        let place = anchor.convert(anchor.bounds, to: container)
        let x = min(place.minX, container.bounds.width - size.width - 8)
        let y = container.isFlipped ? place.minY - 10 - size.height : place.maxY + 10
        frame = NSRect(x: x, y: y, width: size.width, height: size.height)
        isHidden = false
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .keyDown]) {
            [weak self] event in
            guard let self else {
                return event
            }
            if event.type == .keyDown {
                if event.keyCode == 53 {  // Escape
                    close()
                    return nil
                }
                return event
            }
            // A click on its own button is left to the button, which closes it.
            let onAnchor = self.anchor.map { $0.bounds.contains($0.convert(event.locationInWindow, from: nil)) } ?? false
            if !bounds.contains(convert(event.locationInWindow, from: nil)), !onAnchor {
                close()
            }
            return event
        }
    }

    func close() {
        isHidden = true
        if let monitor {
            NSEvent.removeMonitor(monitor)
        }
        monitor = nil
    }
}

/// A row of a menu: its title, 6 above and below and 12 at the sides, after an icon column in menus
/// that have one, and a check mark at its end when chosen; cream under the pointer.
@MainActor
private final class MenuRow: ChromeControl {
    init(title: String, icon: Icons.Name?, iconColumn: Bool, checked: Bool) {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 6
        let label = NSTextField(labelWithString: title)
        label.font = Typography.font(Typography.textSmall)
        label.textColor = Palette.ink
        let views: [NSView] = [
            iconColumn ? Self.imageView(icon.map { Icons.image($0, size: 15) }, size: 15) : nil,
            label,
            checked ? Self.imageView(Icons.image(.check, size: 13), size: 15) : nil,
        ].compactMap { $0 }
        let row = NSStackView(views: views)
        row.orientation = .horizontal
        row.spacing = 8
        row.translatesAutoresizingMaskIntoConstraints = false
        addSubview(row)
        setAccessibilityRole(.menuItem)
        setAccessibilityLabel(title)
        NSLayoutConstraint.activate([
            row.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            row.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -12),
            row.topAnchor.constraint(equalTo: topAnchor, constant: 6),
            row.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
        ])
        if checked, let mark = views.last {
            // The check sits at the row's end, whatever the title's length.
            row.removeArrangedSubview(mark)
            mark.removeFromSuperview()
            mark.translatesAutoresizingMaskIntoConstraints = false
            addSubview(mark)
            NSLayoutConstraint.activate([
                mark.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
                mark.centerYAnchor.constraint(equalTo: centerYAnchor),
                mark.leadingAnchor.constraint(greaterThanOrEqualTo: row.trailingAnchor, constant: 12),
            ])
        }
    }

    private static func imageView(_ image: NSImage?, size: CGFloat) -> NSView {
        let view = NSImageView()
        view.image = image
        view.contentTintColor = Palette.ink
        view.translatesAutoresizingMaskIntoConstraints = false
        view.widthAnchor.constraint(equalToConstant: size).isActive = true
        return view
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("MenuRow is not used from Interface Builder")
    }

    override func stateChanged() {
        alphaValue = isEnabled ? 1 : 0.4
        layer?.backgroundColor = (isHovering && isEnabled ? Palette.cream : .clear).cgColor
    }
}

/// A heading over a menu's rows: muted, small, spaced as the card titles are.
@MainActor
private final class MenuHeading: NSView {
    init(title: String) {
        super.init(frame: .zero)
        let label = NSTextField(labelWithString: title)
        label.font = Typography.font(Typography.textExtraSmall, .medium)
        label.textColor = Palette.muted
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            label.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -12),
            label.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            label.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -4),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("MenuHeading is not used from Interface Builder")
    }
}
