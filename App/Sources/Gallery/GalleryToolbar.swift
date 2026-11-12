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
    var onPoster: (() -> Void)?
    /// Ladders' scale chosen: true for one shared by every strip.
    var onLadders: ((Bool) -> Void)?
    /// Atlas: a level chosen, or a step in (1) or out (−1).
    var onAtlasLevel: ((Int) -> Void)?
    var onAtlasStep: ((Int) -> Void)?
    /// Catalogue: a step back (−1) or on (1), a spread, or the index (neither).
    var onCatalogue: ((_ step: Int?, _ spread: Int?) -> Void)?

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
    private let poster = PillButton(
        title: "Poster",
        icon: .poster,
        help: "Show it off: the view as a poster (⌥⌘P)"
    )
    private let sheetControls: NSStackView
    private let laddersControls: NSStackView
    private let atlasControls: NSStackView
    private let atlasLevels = SegmentedPill(
        ["Regions", "Districts", "Parts"],
        selected: 0,
        help: "Level"
    )
    private let atlasOut = PillButton(title: "\u{2212}", help: "Zoom out a level (\u{2212})")
    private let atlasIn = PillButton(title: "+", help: "Zoom in a level (+)")
    private let catalogueControls: NSStackView
    private let pages = NSTextField(labelWithString: "")
    private let contents = PillButton(title: "Contents", help: "The contents")
    private let index = PillButton(title: "Index", help: "The index")
    private let stack = NSStackView(views: [])
    /// The room the toolbar has, as the window last gave it.
    private var available: CGFloat = .greatestFiniteMagnitude
    private let laddersSort = SegmentedPill(["Size", "Name"], selected: 0, help: "Order")
    private let laddersScale = SegmentedPill(
        ["Each", "Shared"],
        selected: 0,
        help: "Scale",
        tips: ["Each strip at its own scale", "Every strip at one scale"]
    )
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
        laddersControls = NSStackView(views: [])
        atlasControls = NSStackView(views: [])
        catalogueControls = NSStackView(views: [])
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
        for view in [Self.rule(), label("SORT"), laddersSort, label("SCALE"), laddersScale] {
            laddersControls.addArrangedSubview(view)
        }
        laddersControls.orientation = .horizontal
        laddersControls.spacing = 8
        for view in [Self.rule(), atlasOut, atlasLevels, atlasIn] {
            atlasControls.addArrangedSubview(view)
        }
        atlasControls.orientation = .horizontal
        atlasControls.spacing = 8
        atlasLevels.onChange = { [weak self] level in self?.onAtlasLevel?(level) }
        atlasOut.onPress = { [weak self] in self?.onAtlasStep?(-1) }
        atlasIn.onPress = { [weak self] in self?.onAtlasStep?(1) }
        // Catalogue: turn the book, the pages in view, the contents and the index.
        pages.font = Typography.font(Typography.textExtraSmall, .semibold)
        pages.textColor = Palette.muted
        let turnBack = PillButton(title: "\u{2039}", help: "Turn back (\u{2190})")
        let turnOn = PillButton(title: "\u{203A}", help: "Turn on (\u{2192})")
        for view in [Self.rule(), turnBack, pages, turnOn, contents, index] {
            catalogueControls.addArrangedSubview(view)
        }
        catalogueControls.orientation = .horizontal
        catalogueControls.spacing = 8
        turnBack.onPress = { [weak self] in self?.onCatalogue?(-1, nil) }
        turnOn.onPress = { [weak self] in self?.onCatalogue?(1, nil) }
        contents.onPress = { [weak self] in self?.onCatalogue?(nil, 1) }
        index.onPress = { [weak self] in self?.onCatalogue?(nil, nil) }
        laddersSort.onChange = { [weak self] index in self?.change { $0.sort = index == 0 ? 1 : 0 }
        }
        laddersScale.onChange = { [weak self] index in self?.onLadders?(index == 1) }
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
        poster.onPress = { [weak self] in self?.onPoster?() }

        for view in [
            viewSwitch, sheetControls, laddersControls, atlasControls, catalogueControls,
            Self.rule(), poster, look,
        ] {
            stack.addArrangedSubview(view)
        }
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
    func show(
        view: UInt32,
        sheet: SheetOptions,
        style: String,
        laddersShared: Bool = false,
        atlasLevel: Int = 0,
        catalogue: GalleryHandle.Catalogue = .init()
    ) {
        catalogueControls.isHidden = view != GeoViewCatalogue.rawValue
        contents.isOn = catalogue.spread == 1
        index.isOn = catalogue.spread > 0 && catalogue.spread >= catalogue.index
        let total = catalogue.spreads * 2
        pages.stringValue =
            catalogue.spread == 0
            ? "Cover \u{00B7} \(total - 2) pages"
            : "Pages \(catalogue.spread * 2)\u{2013}\(catalogue.spread * 2 + 1) of \(total - 1)"
        atlasControls.isHidden = view != GeoViewAtlas.rawValue
        atlasLevels.selected = atlasLevel
        laddersControls.isHidden = view != GeoViewLadders.rawValue
        laddersSort.selected = sheet.sort == GeoSortSize.rawValue ? 0 : 1
        laddersScale.selected = laddersShared ? 1 : 0
        if let index = views.firstIndex(of: view) {
            viewSwitch.selected = index
        }
        sheetControls.isHidden = view != GeoViewSheet.rawValue
        options = sheet
        scale.selected = Int(sheet.scale)
        sort.selected = Int(sheet.sort)
        size.value = Double((sheet.cell - 72) / 128)
        look.title = style
        fit(available: available)
    }

    /// How far the toolbar runs past its room, and how many folds it took to fit.
    var cut: CGFloat { max(0, stack.fittingSize.width + 20 - available) }
    var folds: Int { (viewSwitch.showsKeys ? 0 : 1) + (look.titleHidden ? 1 : 0) }

    /// Fits the toolbar into `available` points, as the page does (gallery-toolbar.js): short of
    /// room, the view switch drops its keys, then Poster and the Look pill their titles.
    func fit(available: CGFloat) {
        self.available = available
        let folds: [(Bool) -> Void] = [
            { [viewSwitch] in viewSwitch.showsKeys = !$0 },
            { [poster, look] in
                poster.titleHidden = $0
                look.titleHidden = $0
            },
        ]
        for fold in folds {
            fold(false)
        }
        for fold in folds where stack.fittingSize.width + 20 > available {
            fold(true)
        }
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
