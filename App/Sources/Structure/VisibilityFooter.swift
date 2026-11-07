import AppKit

/// The foot of the Structure card while anything is hidden or ghosted: how much, and the quickest
/// way back to the whole model.
@MainActor
final class VisibilityFooter: NSView {
    var onShowAll: (() -> Void)?
    var onUnghostAll: (() -> Void)?

    private let rule = NSView()
    private let label = NSTextField(labelWithString: "")
    private let showAll = PillButton(title: "Show all", help: "Show everything hidden (⇧H)")
    private let unghost = PillButton(title: "Unghost", help: "Unghost everything (⇧G)")

    override init(frame: NSRect) {
        super.init(frame: frame)
        rule.wantsLayer = true
        rule.layer?.backgroundColor = Palette.rule.cgColor
        label.font = Typography.font(Typography.textSmall, .medium)
        label.textColor = Palette.ink
        label.lineBreakMode = .byTruncatingTail
        showAll.onPress = { [weak self] in self?.onShowAll?() }
        unghost.onPress = { [weak self] in self?.onUnghostAll?() }

        let buttons = NSStackView(views: [showAll, unghost])
        buttons.spacing = 8
        for view in [rule, label, buttons] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            addSubview(view)
        }
        NSLayoutConstraint.activate([
            rule.leadingAnchor.constraint(equalTo: leadingAnchor),
            rule.trailingAnchor.constraint(equalTo: trailingAnchor),
            rule.topAnchor.constraint(equalTo: topAnchor),
            rule.heightAnchor.constraint(equalToConstant: Metrics.line),
            label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 2),
            label.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor),
            label.topAnchor.constraint(equalTo: rule.bottomAnchor, constant: 10),
            buttons.leadingAnchor.constraint(equalTo: leadingAnchor),
            buttons.topAnchor.constraint(equalTo: label.bottomAnchor, constant: 8),
            buttons.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("VisibilityFooter is not used from Interface Builder")
    }

    /// Counts are of bodies with anything hidden, and with anything ghosted.
    func show(hidden: UInt32, ghosted: UInt32) {
        let bodies = { (count: UInt32) in count == 1 ? "1 body" : "\(count.formatted()) bodies" }
        switch (hidden > 0, ghosted > 0) {
        case (true, true):
            label.stringValue = "\(hidden.formatted()) hidden · \(ghosted.formatted()) ghosted"
        case (true, false):
            label.stringValue = "\(bodies(hidden)) hidden"
        case (false, true):
            label.stringValue = "\(bodies(ghosted)) ghosted"
        case (false, false):
            label.stringValue = ""
        }
        showAll.isHidden = hidden == 0
        unghost.isHidden = ghosted == 0
    }
}
