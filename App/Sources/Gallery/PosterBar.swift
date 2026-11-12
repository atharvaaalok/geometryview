import AppKit

/// While a poster shows: its buttons in the top right margin, outside the printed picture. Copy
/// puts the poster on the pasteboard, Save PNG and Save PDF save it, Done (or Esc) goes back. The
/// Mac's counterpart of the page's poster bar (gallery-toolbar.js).
@MainActor
final class PosterBar: StickerCard {
    var onCopy: (() -> Void)?
    var onSavePNG: (() -> Void)?
    var onSavePDF: (() -> Void)?
    var onDone: (() -> Void)?

    private let copy = PillButton(title: "Copy", help: "Copy the poster as a picture (⌘C)")
    private let savePNG = PillButton(title: "Save PNG", help: "Save the poster as a picture (⌘S)")
    private let savePDF = PillButton(
        title: "Save PDF",
        help: "Save the poster, or in Catalogue the whole book, as a PDF"
    )
    private let done = PillButton(title: "Done", help: "Back to the gallery (Esc)")

    init() {
        super.init(radius: Metrics.radiusChip, shadow: Metrics.shadowChip)
        copy.onPress = { [weak self] in self?.onCopy?() }
        savePNG.onPress = { [weak self] in self?.onSavePNG?() }
        savePDF.onPress = { [weak self] in self?.onSavePDF?() }
        done.onPress = { [weak self] in self?.onDone?() }
        let stack = NSStackView(views: [copy, savePNG, savePDF, done])
        stack.orientation = .horizontal
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 3),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -3),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 3),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -3),
        ])
        isHidden = true
        setAccessibilityLabel("Poster")
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("PosterBar is not used from Interface Builder")
    }
}
