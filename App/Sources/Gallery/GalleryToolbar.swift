import AppKit
import GeometryViewEngine

/// The gallery's toolbar, in place of the viewer's while the gallery shows: the view switch, the
/// view's own controls (Sheet's scale, picture size and order), and the Look pill. The Mac's
/// counterpart of Web/viewer/gallery-toolbar.js.
@MainActor
final class GalleryToolbar: StickerCard {
    /// Called with the view chosen, Sheet's controls changed, or the Look pill pressed.
    var onView: ((UInt32) -> Void)?
    var onSheet: ((SheetOptions) -> Void)?
    var onShowLook: (() -> Void)?

    private static let names = ["Plates", "Sheet", "Ladders", "Atlas", "Catalogue"]
    private static let icons: [Icons.Name] = [.plates, .sheet, .ladders, .atlas, .catalogue]

    private let views: [UInt32]
    private let viewSwitch: SegmentedPill
    private let scale = SegmentedPill(
        ["Fit", "Folder", "True"],
        selected: 1,
        help: "How parts are scaled",
        tips: [
            "Each part fills its cell", "One scale for each folder",
            "One scale for everything in view",
        ]
    )
    private let size = StickerSlider(help: "Picture size")
    private let sort = SegmentedPill(["Name", "Size"], selected: 0, help: "Order")
    private let look = PillButton(title: "Illustration", icon: .palette, help: "Look (⇧⌘L)")
    private let sheetControls: NSStackView
    private var options = SheetOptions()

    /// `enabled`: the views that may be shown, a bit each.
    init(enabled: UInt32) {
        views = (0..<5).map(UInt32.init).filter { enabled & (1 << $0) != 0 }
        viewSwitch = SegmentedPill(
            views.map { Self.names[Int($0)] },
            selected: 0,
            help: "View",
            tips: views.map { "\(Self.names[Int($0)]) (⌘\($0 + 1))" },
            icons: views.map { Self.icons[Int($0)] },
            keys: views.map { "⌘\($0 + 1)" }
        )
        sheetControls = NSStackView(views: [])
        super.init()
        let label = { (text: String) -> NSTextField in
            let field = NSTextField(labelWithString: "")
            field.attributedStringValue = NSAttributedString(
                string: text,
                attributes: [
                    .font: Typography.font(Typography.textExtraSmall, .semibold),
                    .foregroundColor: Palette.muted,
                    .kern: Typography.textExtraSmall * 0.06,
                ]
            )
            return field
        }
        for view in [Self.rule(), label("SCALE"), scale, label("SIZE"), size, label("SORT"), sort] {
            sheetControls.addArrangedSubview(view)
        }
        sheetControls.orientation = .horizontal
        sheetControls.spacing = 8
        size.widthAnchor.constraint(equalToConstant: 110).isActive = true

        viewSwitch.onChange = { [weak self] index in
            guard let self else {
                return
            }
            onView?(views[index])
        }
        scale.onChange = { [weak self] index in self?.change { $0.scale = UInt32(index) } }
        sort.onChange = { [weak self] index in self?.change { $0.sort = UInt32(index) } }
        size.onChange = { [weak self] value in
            self?.change { $0.cell = ((72 + value * 128) / 4).rounded() * 4 }
        }
        look.onPress = { [weak self] in self?.onShowLook?() }

        let stack = NSStackView(views: [viewSwitch, sheetControls, Self.rule(), look])
        stack.orientation = .horizontal
        stack.spacing = 8
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -8),
        ])
        setAccessibilityLabel("Gallery")
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("GalleryToolbar is not used from Interface Builder")
    }

    /// Shows the view in use, Sheet's controls, and the style's name.
    func show(view: UInt32, sheet: SheetOptions, style: String) {
        if let index = views.firstIndex(of: view) {
            viewSwitch.selected = index
        }
        sheetControls.isHidden = view != GeoViewSheet.rawValue
        options = sheet
        scale.selected = Int(sheet.scale)
        sort.selected = Int(sheet.sort)
        size.value = Double((sheet.cell - 72) / 128)
        look.title = style
    }

    private func change(_ edit: (inout SheetOptions) -> Void) {
        edit(&options)
        onSheet?(options)
    }

    private static func rule() -> NSView {
        let view = NSView()
        view.wantsLayer = true
        view.layer?.backgroundColor = Palette.rule.cgColor
        view.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            view.widthAnchor.constraint(equalToConstant: Metrics.line),
            view.heightAnchor.constraint(equalToConstant: 22),
        ])
        return view
    }
}
