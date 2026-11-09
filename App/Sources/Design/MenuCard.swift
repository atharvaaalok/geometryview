import AppKit

/// A menu as the page draws one, rather than the system's: a small sticker card of plain rows,
/// cream under the pointer. It opens above what it belongs to and closes when a row is chosen, on
/// Escape, or on a click anywhere else.
@MainActor
final class MenuCard: StickerCard {
    private var monitor: Any?
    private weak var anchor: NSView?

    init(items: [(title: String, action: () -> Void)]) {
        super.init(radius: Metrics.radiusChip, shadow: Metrics.shadowChip)
        isHidden = true
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 0
        for (title, action) in items {
            let row = MenuRow(title: title)
            row.onPress = { [weak self] in
                self?.close()
                action()
            }
            stack.addArrangedSubview(row)
            row.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
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

/// A row of a menu: its title, 6 above and below and 12 at the sides, on cream under the pointer.
@MainActor
private final class MenuRow: ChromeControl {
    private let label = NSTextField(labelWithString: "")

    init(title: String) {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 6
        label.stringValue = title
        label.font = Typography.font(Typography.textSmall)
        label.textColor = Palette.ink
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        setAccessibilityRole(.menuItem)
        setAccessibilityLabel(title)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12 - 2),
            label.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -12 + 2),
            label.topAnchor.constraint(equalTo: topAnchor, constant: 6),
            label.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("MenuRow is not used from Interface Builder")
    }

    override func stateChanged() {
        layer?.backgroundColor = (isHovering ? Palette.cream : .clear).cgColor
    }
}
