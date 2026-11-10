import AppKit
import GeometryViewEngine

/// Shown while H or G is held, so the changed meaning of a click is never a surprise.
@MainActor
final class HoldHint: NSView {
    private let chip = Chip()

    override init(frame: NSRect) {
        super.init(frame: frame)
        chip.translatesAutoresizingMaskIntoConstraints = false
        addSubview(chip)
        NSLayoutConstraint.activate([
            chip.leadingAnchor.constraint(equalTo: leadingAnchor),
            chip.trailingAnchor.constraint(equalTo: trailingAnchor),
            chip.topAnchor.constraint(equalTo: topAnchor),
            chip.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        isHidden = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("HoldHint is not used from Interface Builder")
    }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    func show(_ tool: GeoTool) {
        switch tool {
        case GeoToolHide:
            chip.fillColor = Palette.tomatoWash
            chip.setContent([
                KeyCap("H"), Chip.text("Click to hide. Click again to peel the next layer."),
            ])
            isHidden = false
        case GeoToolGhost:
            chip.fillColor = Palette.sheet
            chip.setContent([
                KeyCap("G"), Chip.text("Click to ghost. Click a ghost to bring it back."),
            ])
            isHidden = false
        default:
            isHidden = true
        }
    }
}

/// The dashed frame shown while files are dragged over the window.
@MainActor
final class DropZone: NSView {
    private let border = CAShapeLayer()

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = Palette.cream.withAlphaComponent(0.7).cgColor
        layer?.cornerRadius = 18
        border.fillColor = nil
        border.strokeColor = Palette.ink.cgColor
        border.lineWidth = 2
        border.lineDashPattern = [8, 6]
        layer?.addSublayer(border)
        let label = Chip.text(
            "Drop files to open them, or a folder to connect it. Hold ⌘ to add to what is shown.",
            weight: .semibold
        )
        label.font = Typography.font(Typography.textLarge, .semibold)
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        NSLayoutConstraint.activate([
            label.centerXAnchor.constraint(equalTo: centerXAnchor),
            label.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
        isHidden = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("DropZone is not used from Interface Builder")
    }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    override func layout() {
        super.layout()
        border.path = CGPath(
            roundedRect: bounds.insetBy(dx: 1, dy: 1),
            cornerWidth: 18,
            cornerHeight: 18,
            transform: nil
        )
        border.frame = bounds
    }
}

/// Every key the viewer understands, from the same tables the keys are read from.
@MainActor
final class ShortcutsSheet: NSView {
    var onClose: (() -> Void)?

    private let grid = NSGridView()

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = Palette.paper.withAlphaComponent(0.55).cgColor

        let card = StickerCard()
        card.translatesAutoresizingMaskIntoConstraints = false
        addSubview(card)
        let title = NSTextField(labelWithString: "Shortcuts")
        title.font = Typography.font(Typography.textLarge, .semibold)
        title.textColor = Palette.ink

        grid.rowSpacing = 8
        grid.columnSpacing = 12
        fillGrid()
        KeyMap.shared.onChange = { [weak self] in self?.fillGrid() }

        let stack = NSStackView(views: [title, grid])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 14
        stack.translatesAutoresizingMaskIntoConstraints = false
        card.addSubview(stack)
        NSLayoutConstraint.activate([
            card.centerXAnchor.constraint(equalTo: centerXAnchor),
            card.centerYAnchor.constraint(equalTo: centerYAnchor),
            stack.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -20),
            stack.topAnchor.constraint(equalTo: card.topAnchor, constant: 18),
            stack.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -18),
        ])
        isHidden = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("ShortcutsSheet is not used from Interface Builder")
    }

    override func mouseDown(with event: NSEvent) {
        onClose?()
    }

    /// The rows, with keys read from the key map, so a rebound key shows as it is now.
    private func fillGrid() {
        while grid.numberOfRows > 0 {
            grid.removeRow(at: 0)
        }
        let keys = KeyMap.shared
        let cap = { (action: KeyAction) in keys.chord(for: action)?.label ?? "—" }
        let entries: [([String], String)] = [
            (["Click"], "Select. Shift or ⌘ adds"),
            (
                [cap(.faceMode), cap(.bodyMode), cap(.partMode)],
                "Clicks select faces, bodies or parts"
            ),
            ([cap(.holdHide), "Click"], "Hide what you click"),
            ([cap(.holdGhost), "Click"], "Ghost, or bring back a ghost"),
            ([cap(.showAll)], "Show everything"),
            ([cap(.unghostAll)], "Unghost everything"),
            ([cap(.isolate)], "Isolate the selection"),
            (["⌘", "Z"], "Undo, with ⇧ to redo"),
            ([cap(.fit)], "Fit to view"),
            ([cap(.toggleProjection)], "Orthographic or perspective"),
            ([cap(.seeThrough)], "See through the housing"),
            ([cap(.section)], "Section across X"),
            ([cap(.clearSelection)], "Clear the selection"),
            (["⌘", "W"], "Close the file"),
            ([cap(.shortcuts)], "This sheet"),
        ]
        for (keyLabels, meaning) in entries {
            let caps = NSStackView(views: keyLabels.map { KeyCap($0) })
            caps.spacing = 4
            grid.addRow(with: [caps, Chip.text(meaning)])
        }
        grid.column(at: 0).xPlacement = .trailing
    }
}
