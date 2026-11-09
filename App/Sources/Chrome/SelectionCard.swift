import AppKit
import GeometryViewEngine

/// What is selected: its name and place in the assembly, how much of it, and what to do with it.
@MainActor
final class SelectionCard: StickerCard {
    private let kind = NSTextField(labelWithString: "")
    private let name = NSTextField(wrappingLabelWithString: "")
    private let path = NSTextField(wrappingLabelWithString: "")
    private let faces = Stat(unit: "faces")
    private let bodies = Stat(unit: "bodies")

    var onAction: ((GeoAction) -> Void)?

    init() {
        super.init()
        kind.font = Typography.font(Typography.textExtraSmall, .medium)
        kind.textColor = Palette.muted
        name.font = Typography.font(Typography.textLarge, .semibold)
        name.textColor = Palette.ink
        path.font = Typography.font(Typography.textExtraSmall)
        path.textColor = Palette.muted
        for label in [name, path] {
            label.preferredMaxLayoutWidth = 256
            label.alignment = .left
        }

        let head = NSStackView(views: [kind, name, path])
        head.orientation = .vertical
        head.alignment = .leading
        head.spacing = 3

        let stats = NSStackView(views: [faces, bodies])
        stats.orientation = .horizontal
        stats.distribution = .fillEqually
        stats.alignment = .top
        stats.spacing = 16

        let hide = PillButton(title: "Hide", help: "Hide the selection")
        let ghost = PillButton(title: "Ghost", help: "Ghost the selection")
        let isolate = PillButton(title: "Isolate", help: "Hide everything else")
        let fit = PillButton(title: "Fit to selection", help: "Frame the selection")
        for (button, action) in [
            (hide, GeoActionHideSelection), (ghost, GeoActionGhostSelection),
            (isolate, GeoActionIsolate), (fit, GeoActionFitSelection),
        ] {
            button.onPress = { [weak self] in self?.onAction?(action) }
        }
        let row = NSStackView(views: [hide, ghost, isolate])
        row.orientation = .horizontal
        row.distribution = .fillEqually
        row.spacing = 6
        let actions = NSStackView(views: [row, fit])
        actions.orientation = .vertical
        actions.spacing = 6
        actions.alignment = .width
        for button in [hide, ghost, isolate, fit] {
            button.setContentHuggingPriority(.defaultLow, for: .horizontal)
        }

        let rule = NSBox()
        rule.boxType = .custom
        rule.borderWidth = 0
        rule.fillColor = Palette.ink
        let stack = NSStackView(views: [head, rule, stats, actions])
        stack.orientation = .vertical
        stack.alignment = .width
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            rule.heightAnchor.constraint(equalToConstant: Metrics.line),
            // Every section spans the card, so text starts at its left edge.
            head.widthAnchor.constraint(equalTo: stack.widthAnchor),
            rule.widthAnchor.constraint(equalTo: stack.widthAnchor),
            stats.widthAnchor.constraint(equalTo: stack.widthAnchor),
            actions.widthAnchor.constraint(equalTo: stack.widthAnchor),
            row.widthAnchor.constraint(equalTo: actions.widthAnchor),
            fit.widthAnchor.constraint(equalTo: actions.widthAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 14),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 12),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -14),
        ])
        isHidden = true
    }

    /// Shows the summary, naming the focus row from `tree`, or hides the card if nothing is
    /// selected.
    func show(_ summary: GeoSelectionSummary, tree: TreeRows) {
        guard summary.occurrences > 0 else {
            isHidden = true
            return
        }
        let mode = ["FACE", "BODY", "PART"][Int(min(summary.mode, 2))]
        kind.stringValue =
            summary.occurrences > 1 ? "\(mode) · \(summary.occurrences) BODIES" : mode
        if summary.node != UInt32.max {
            let row = TreeRow(document: summary.document, node: summary.node, depth: 0)
            name.stringValue = tree.name(of: row)
            path.stringValue = tree.path(of: row)
        } else {
            name.stringValue = "Selection"
            path.stringValue = ""
        }
        faces.value = summary.faces
        bodies.value = summary.occurrences
        isHidden = false
    }

    @MainActor
    private final class Stat: NSView {
        var value: UInt32 = 0 {
            didSet { number.stringValue = value.formatted() }
        }
        private let number = NSTextField(labelWithString: "0")

        init(unit: String) {
            super.init(frame: .zero)
            number.font = Typography.font(Typography.textStat, .semibold)
            number.textColor = Palette.ink
            let label = NSTextField(labelWithString: unit)
            label.font = Typography.font(Typography.textSmall, .medium)
            label.textColor = Palette.inkSoft
            let stack = NSStackView(views: [number, label])
            stack.orientation = .vertical
            stack.alignment = .leading
            stack.spacing = 0
            stack.translatesAutoresizingMaskIntoConstraints = false
            addSubview(stack)
            NSLayoutConstraint.activate([
                stack.leadingAnchor.constraint(equalTo: leadingAnchor),
                stack.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor),
                stack.topAnchor.constraint(equalTo: topAnchor),
                stack.bottomAnchor.constraint(equalTo: bottomAnchor),
            ])
        }

        @available(*, unavailable)
        required init?(coder: NSCoder) {
            fatalError("Stat is not used from Interface Builder")
        }
    }
}
