import AppKit
import GeometryViewEngine

/// The gallery's checks, from the command line:
///
///   --gallery <folder>   opens a connected folder's gallery at launch
///   --gallery-check      then measures every alignment group as painted, in Illustration,
///                        Blueprint and Studio, in the folder and its first subfolders; counts the
///                        3D frames drawn while the gallery covers the view; and checks that IBM
///                        Plex Mono's ascent is the 1.025 em the layout reckons with. Writes
///                        gallery-check.json to the temporary folder and quits.
///   --gallery-layout     writes the folder's display list, in Illustration, as
///                        gallery-layout.json, for comparing with the browser's, and quits
///   --gallery-settle <warm|cold>
///                        opens the folder in Plates and watches it settle: every 50 ms, where
///                        each part is drawn. Counts parts that moved, parts that went (a
///                        cover's pick swapped, say), scale bars that changed, and when the last
///                        change came; writes them to gallery-settle.json and quits. "cold" is a
///                        label: run it with GEOMETRYVIEW_GALLERY_CACHES naming an empty folder.
///
/// The folder must be inside a connected folder, as the app reaches only those.
@MainActor
final class GalleryCheck {
    static var folder: String? {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--gallery"), at + 1 < arguments.count else {
            return nil
        }
        return arguments[at + 1]
    }
    static var isCheck: Bool { CommandLine.arguments.contains("--gallery-check") }
    /// The views the gallery may show: all five, or Plates and Sheet alone with
    /// `--gallery-views two`. (`--gallery-views all`, which the checks pass, shows all five.)
    static var views: UInt32 {
        let arguments = CommandLine.arguments
        if let at = arguments.firstIndex(of: "--gallery-views"), at + 1 < arguments.count,
            arguments[at + 1] == "two"
        {
            return (1 << GeoViewPlates.rawValue) | (1 << GeoViewSheet.rawValue)
        }
        return 0b11111
    }
    static var isLayout: Bool { CommandLine.arguments.contains("--gallery-layout") }
    /// `--gallery-settle warm|cold`: watches the folder settle, and quits.
    static var settleMode: String? {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--gallery-settle"), at + 1 < arguments.count else {
            return nil
        }
        return arguments[at + 1]
    }
    /// `--gallery-poster`: the folder as a poster in each view, checked and saved as PNG.
    static var isPoster: Bool { CommandLine.arguments.contains("--gallery-poster") }
    /// Shows or puts away the poster, as the window does.
    var usePoster: ((Bool) -> Void)?
    /// The window's own, for checks that need it: the Look card, whether the gallery shows, and
    /// the masthead's ×.
    struct Hooks {
        var toggleLook: () -> Void
        var lookOpen: () -> Bool
        var inGallery: () -> Bool
        var pressClose: () -> Void
    }
    var hooks: Hooks?
    /// `--gallery-watch <file>`: shows the file's folder, says when it is ready, and waits for the
    /// file to change on disk, to see it re-ink alone.
    static var watchFile: String? {
        let arguments = CommandLine.arguments
        guard let at = arguments.firstIndex(of: "--gallery-watch"), at + 1 < arguments.count else {
            return nil
        }
        return arguments[at + 1]
    }

    private let controller: GalleryController
    private let engine: EngineHandle
    private let useStyle: (GeoStyle) -> Void

    init(
        controller: GalleryController,
        engine: EngineHandle,
        useStyle: @escaping (GeoStyle) -> Void
    ) {
        self.controller = controller
        self.engine = engine
        self.useStyle = useStyle
    }

    func run(root: URL, folder: URL) async {
        var report: [String: Any] = ["folder": folder.path]
        // Plex Mono's ascent, which the layout's baselines assume.
        let font = Typography.font(10)
        report["ascentEm"] = Double(font.ascender / 10)
        report["descentEm"] = Double(-font.descender / 10)

        if Self.isLayout {
            useStyle(GeoStyleIllustration)
            controller.show(root: root, folder: folder)
            let waited = await settle()
            report["layout"] = layoutDump()
            report["layoutWaited"] = waited
            write(
                [
                    "width": Double(controller.view.contentArea.width),
                    "height": Double(controller.view.contentArea.height),
                    "folder": controller.gallery.folder, "items": layoutDump(),
                ],
                to: "gallery-layout.json"
            )
            if !Self.isCheck {
                NSApp.terminate(nil)
                return
            }
        }

        if let mode = Self.settleMode {
            report["settle"] = await watchSettle(root: root, folder: folder, mode: mode)
            write(report, to: "gallery-settle.json")
            NSApp.terminate(nil)
            return
        }

        if Self.isPoster {
            report["posters"] = await posters(root: root, folder: folder)
            write(report, to: "gallery-check.json")
            NSApp.terminate(nil)
            return
        }

        if let watched = Self.watchFile {
            report["watch"] = await watch(root: root, file: URL(fileURLWithPath: watched))
            write(report, to: "gallery-check.json")
            NSApp.terminate(nil)
            return
        }

        // The folder, then its first subfolders, once the scan has listed them.
        controller.show(root: root, folder: folder)
        let scanStart = Date()
        while controller.scanning, Date().timeIntervalSince(scanStart) < 60 {
            try? await Task.sleep(for: .milliseconds(100))
        }
        report["scanSeconds"] = Date().timeIntervalSince(scanStart)
        var folders = [folder]
        for sub in controller.subfolders().prefix(3) {
            folders.append(root.appendingPathComponent(sub))
        }
        var runs: [[String: Any]] = []
        let names = ["plates", "sheet", "ladders", "atlas", "catalogue"]
        let views = (0..<5).map(UInt32.init).filter { controller.gallery.views & (1 << $0) != 0 }
        for style in [GeoStyleIllustration, GeoStyleBlueprint, GeoStyleStudio] {
            useStyle(style)
            for view in views {
                for path in folders {
                    // A folder shows the view it last showed: the view is chosen once in it.
                    controller.show(root: root, folder: path)
                    controller.setView(view)
                    try? await Task.sleep(for: .milliseconds(600))  // past any move
                    let waited = await settle()
                    if path == folders.first {
                        snapshot("gallery-\(names[Int(view)])-\(style.rawValue).png")
                    }
                    var run = measure()
                    run["style"] = Int(style.rawValue)
                    run["view"] = Int(view)
                    run["folder"] = controller.gallery.folder
                    run["waitedSeconds"] = waited
                    run["waitingInView"] = controller.waitingInView
                    runs.append(run)
                    if view == GeoViewCatalogue.rawValue {
                        // The book's other spreads: the contents, an opener or a plate, the middle, the index.
                        let spreads = controller.gallery.catalogue.spreads
                        for spread in Set([1, 2, 3, spreads / 2, spreads - 1]).sorted()
                        where spread > 0 && spread < spreads {
                            controller.catalogue(spread: spread)
                            _ = await settle()
                            if path == folders.first {
                                snapshot("gallery-catalogue-\(style.rawValue)-s\(spread).png")
                            }
                            var turned = measure()
                            turned["style"] = Int(style.rawValue)
                            turned["view"] = Int(view)
                            turned["spread"] = spread
                            turned["folder"] = controller.gallery.folder
                            runs.append(turned)
                        }
                        controller.catalogue(spread: 0)
                    }
                    if view == GeoViewAtlas.rawValue {
                        // Atlas closer in: the districts, then every part named.
                        for level in [1, 2] {
                            controller.atlas(level: level)
                            _ = await settle()
                            var closer = measure()
                            closer["style"] = Int(style.rawValue)
                            closer["view"] = Int(view)
                            closer["level"] = level
                            closer["folder"] = controller.gallery.folder
                            runs.append(closer)
                        }
                    }
                    print(
                        "gallery-check: style \(style.rawValue) \(names[Int(view)]) \(controller.gallery.folder.isEmpty ? "/" : controller.gallery.folder): \(run["groupsChecked"] ?? 0) groups, \((run["over"] as? [Any])?.count ?? 0) over 0.5 pt, waited \(String(format: "%.1f", waited)) s"
                    )
                }
            }
        }
        report["g3"] = await motionChecks(root: root, folder: folder, folders: folders)
        // No 3D frames while the gallery covers the view.
        let before = engine.stats().frames_rendered
        try? await Task.sleep(for: .seconds(3))
        let after = engine.stats().frames_rendered
        report["framesWhileCovered"] = Int(after - before)
        report["runs"] = runs
        report["groupsOver"] = failures
        report["worstSpread"] = Double(worst)
        report["maxDeviation"] = Double(worstDeviation)
        if let stats = controller.stats() {
            report["library"] = [
                "helpers": stats.helpers, "imports": stats.imports, "pictures": stats.pictures,
                "lookups": stats.lookups, "failures": stats.failures,
            ]
        }
        report["window"] = await windowChecks(root: root, folder: folder)
        write(report, to: "gallery-check.json")
        print(
            "gallery-check: \(failures) groups over 0.5 pt, \(after - before) 3D frames while covered"
        )
        NSApp.terminate(nil)
    }

    private var worst: Float = 0
    private var worstDeviation: Float = 0
    private var failures = 0

    /// Every alignment group as painted, and how far each item was painted from where the core
    /// laid it.
    private func measure() -> [String: Any] {
        let (lines, columns) = controller.view.measured()
        let result = controller.gallery.check(lines: lines, columns: columns, tolerance: 0.5)
        let over = result.over.map {
            [
                "group": $0.group, "column": $0.column, "spread": Double($0.spread),
                "items": $0.items,
            ]
                as [String: Any]
        }
        var deviation: Float = 0
        for (index, item) in controller.gallery.items.enumerated() {
            if !lines[index].isNaN {
                deviation = max(deviation, abs(lines[index] - item.line))
            }
            if item.column != 0, !columns[index].isNaN {
                deviation = max(deviation, abs(columns[index] - item.column_line))
            }
        }
        worstDeviation = max(worstDeviation, deviation)
        worst = max(worst, result.over.map(\.spread).max() ?? 0)
        failures += result.over.count
        var run: [String: Any] = [
            "groupsChecked": Int(result.checked), "over": over,
            "itemsPainted": lines.filter { !$0.isNaN }.count, "maxDeviation": Double(deviation),
        ]
        // The toolbar fits its room: folded as it must be, never cut off.
        if let toolbar = Self.find(GalleryToolbar.self, in: controller.view.window?.contentView) {
            run["toolbarCut"] = Double(toolbar.cut)
            run["toolbarFold"] = toolbar.folds
        }
        return run
    }

    private static func find<T: NSView>(_ type: T.Type, in view: NSView?) -> T? {
        guard let view else {
            return nil
        }
        if let found = view as? T {
            return found
        }
        for sub in view.subviews {
            if let found = find(type, in: sub) {
                return found
            }
        }
        return nil
    }

    /// G3's checks: a heading of Sheet's stays at the top as its block scrolls under it; a search
    /// fades what it did not find; Peek is painted over the view and aligned; and each move plays
    /// for as long as the spec says, the selection kept.
    private func motionChecks(root: URL, folder: URL, folders: [URL]) async -> [String: Any] {
        var out: [String: Any] = [:]
        let view = controller.view
        let gallery = controller.gallery
        // Sticky headings, in Sheet at the top, scrolled a third of the way.
        controller.show(root: root, folder: folder)
        controller.setView(GeoViewSheet.rawValue)
        try? await Task.sleep(for: .milliseconds(600))
        _ = await settle()
        view.scroll(to: ((gallery.contentHeight - view.contentArea.height) / 3).rounded())
        var sticky: [String: Any] = measure()
        snapshot("gallery-sticky.png")
        if let stuck = view.stuckHeading() {
            sticky["stickyTop"] = Double(stuck.top)
            sticky["fromViewTop"] = Double(stuck.fromViewTop)
        }
        out["sticky"] = sticky
        view.scroll(to: 0)
        // A search: what it did not find painted at 22%.
        let first = gallery.wanted().first.map { gallery.path(of: $0.file) } ?? ""
        let probe = String((first as NSString).lastPathComponent.prefix(3))
        controller.search(probe)
        var search = measure()
        search["query"] = probe
        search["found"] = gallery.matches.map { Int($0) } ?? -1
        search["fadedLayers"] = view.fadedLayers
        out["search"] = search
        controller.search("")
        // Peek over the first file of the first subfolder, in Sheet, where → selects a file.
        controller.show(root: root, folder: folders.dropFirst().first ?? folder)
        controller.setView(GeoViewSheet.rawValue)
        try? await Task.sleep(for: .milliseconds(600))
        _ = await settle()
        view.press(GeoKeyRight)
        view.press(GeoKeySpace)
        var peek = measure()
        peek["file"] = gallery.peeking.map { gallery.path(of: $0) } ?? ""
        peek["fixedLayers"] = view.fixedLayers
        out["peek"] = peek
        snapshot("gallery-peek.png")
        // Peek's turntable: its frames come, then → turns the part a frame at a time.
        let turnStart = Date()
        while !gallery.peekTurnable, Date().timeIntervalSince(turnStart) < 30 {
            try? await Task.sleep(for: .milliseconds(100))
        }
        var turn = measure()
        turn["seconds"] = Date().timeIntervalSince(turnStart)
        turn["turnable"] = gallery.peekTurnable
        snapshot("gallery-peek-turn-0.png")
        for _ in 0..<6 {
            view.press(GeoKeyRight)  // a quarter turn, at 24 frames
        }
        turn["frameAfterSix"] = view.turnFrameShown
        turn["peekingStill"] = gallery.peeking.map { gallery.path(of: $0) } ?? ""
        snapshot("gallery-peek-turn-6.png")
        out["peekTurn"] = turn
        view.press(GeoKeyEscape)
        // A switch from Plates to Sheet, which shows more: its transition and tracks, and the
        // animations playing. Parts in both fly; those new to Sheet fade in.
        controller.setView(GeoViewPlates.rawValue)
        try? await Task.sleep(for: .milliseconds(700))
        let selected = gallery.selected
        controller.setView(GeoViewSheet.rawValue)
        let t = gallery.transition
        let tracks = gallery.tracks
        out["switch"] =
            [
                "transition": [
                    "kind": Int(t.kind), "duration": Double(t.duration),
                    "fadeOut": Double(t.fade_out),
                ],
                "fly": tracks.filter { $0.kind == GeoTrackFly.rawValue }.count,
                "fadeIn": tracks.filter { $0.kind == GeoTrackFadeIn.rawValue }.count,
                "flyTiming": Array(
                    Set(
                        tracks.filter { $0.kind == GeoTrackFly.rawValue }.map {
                            "\(Int($0.delay))+\(Int($0.duration))"
                        }
                    )
                ),
                "fadeTiming": Array(
                    Set(
                        tracks.filter { $0.kind == GeoTrackFadeIn.rawValue }.map {
                            "\(Int($0.delay.rounded()))+\(Int($0.duration.rounded()))"
                        }
                    )
                ),
                "animations": Array(Set(view.animations())).sorted(),
                "selectionKept": selected != nil && gallery.selected == selected,
            ] as [String: Any]
        try? await Task.sleep(for: .milliseconds(700))
        // Into a folder from its plate, and back out.
        controller.show(root: root, folder: folder)
        controller.setView(GeoViewPlates.rawValue)
        try? await Task.sleep(for: .milliseconds(600))
        _ = await settle()
        if let plate = gallery.items.first(where: { $0.target_kind == GeoTargetFolder.rawValue }) {
            view.click(kind: GeoTargetFolder.rawValue, target: plate.target)
            let into = gallery.transition
            let playing = Array(Set(view.animations())).sorted()
            try? await Task.sleep(for: .milliseconds(600))
            view.press(GeoKeyEscape)
            let back = gallery.transition
            let rect = { (t: GeoGalleryTransition) in
                [t.rect.0, t.rect.1, t.rect.2, t.rect.3].map { Int($0.rounded()) }
            }
            out["zoom"] =
                [
                    "into": [
                        "kind": Int(into.kind), "duration": Double(into.duration),
                        "rect": rect(into),
                    ],
                    "out": [
                        "kind": Int(back.kind), "duration": Double(back.duration),
                        "rect": rect(back),
                    ],
                    "animations": playing,
                ] as [String: Any]
            try? await Task.sleep(for: .milliseconds(600))
        }
        return out
    }

    /// The folder as a poster in each view enabled: aligned as painted, and saved as PNG, with the
    /// picture's size.
    private func posters(root: URL, folder: URL) async -> [[String: Any]] {
        controller.show(root: root, folder: folder)
        let scanStart = Date()
        while controller.scanning, Date().timeIntervalSince(scanStart) < 60 {
            try? await Task.sleep(for: .milliseconds(100))
        }
        var out: [[String: Any]] = []
        let names = ["plates", "sheet", "ladders", "atlas", "catalogue"]
        let views = (0..<5).map(UInt32.init).filter { controller.gallery.views & (1 << $0) != 0 }
        for view in views {
            let name = names[Int(view)]
            controller.setView(view)
            usePoster?(true)
            try? await Task.sleep(for: .milliseconds(600))
            _ = await settle()
            var run = measure()
            run["view"] = name
            run["contentHeight"] = Double(controller.gallery.contentHeight)
            controller.settleNow()
            if let image = controller.view.posterImage() {
                run["pixels"] = [image.width, image.height]
                let output = FileManager.default.temporaryDirectory.appendingPathComponent(
                    "gallery-poster-\(name).png"
                )
                try? NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])?
                    .write(to: output)
            }
            // Its PDF too: a page, or in Catalogue the book, a spread a page.
            let pdfStart = Date()
            if let pdf = await controller.posterPDF() {
                run["pdfBytes"] = pdf.count
                run["pdfSeconds"] = Date().timeIntervalSince(pdfStart)
                try? pdf.write(
                    to: FileManager.default.temporaryDirectory.appendingPathComponent(
                        "gallery-poster-\(name).pdf"
                    )
                )
            }
            out.append(run)
            usePoster?(false)
        }
        return out
    }

    /// A file changed on disk, with its folder in view in Sheet: how long until its Δ shows and its
    /// new picture comes, whether anything else moved, and how many files were imported again.
    private func watch(root: URL, file url: URL) async -> [String: Any] {
        let gallery = controller.gallery
        controller.show(root: root, folder: url.deletingLastPathComponent())
        let scanStart = Date()
        while controller.scanning, Date().timeIntervalSince(scanStart) < 60 {
            try? await Task.sleep(for: .milliseconds(100))
        }
        controller.setView(GeoViewSheet.rawValue)
        try? await Task.sleep(for: .milliseconds(600))
        _ = await settle()
        guard let path = GalleryController.relative(url, to: root),
            let file = gallery.file(at: path)
        else {
            return ["error": "not in the gallery"]
        }
        // Every item but a Δ's, by kind and place.
        let delta = "\u{0394}"
        func layout() -> [String] {
            let items = gallery.items
            var out: [String] = []
            for (index, item) in items.enumerated() {
                if item.kind == GeoItemText.rawValue, gallery.text(of: item) == delta {
                    continue
                }
                // A Δ's chip is the rectangle right before its text.
                if index + 1 < items.count, items[index + 1].kind == GeoItemText.rawValue,
                    gallery.text(of: items[index + 1]) == delta
                {
                    continue
                }
                out.append(
                    String(
                        format: "%u %u %.2f %.2f %.2f %.2f",
                        item.kind,
                        item.file,
                        item.x,
                        item.y,
                        item.width,
                        item.height
                    )
                )
            }
            return out
        }
        func marked() -> Bool {
            gallery.items.contains {
                $0.kind == GeoItemText.rawValue && gallery.text(of: $0) == delta
            }
        }
        func stale() -> Bool {
            gallery.items.contains { $0.flags & GeoItemStale.rawValue != 0 }
        }
        let before = layout()
        let importsBefore = controller.stats()?.imports ?? 0
        print("gallery-watch: ready")
        fflush(stdout)
        let start = Date()
        var markedAfter: Double?
        var staleSeen = false
        var inkedAfter: Double?
        while Date().timeIntervalSince(start) < 120 {
            try? await Task.sleep(for: .milliseconds(50))
            if markedAfter == nil, marked() {
                markedAfter = Date().timeIntervalSince(start)
            }
            staleSeen = staleSeen || stale()
            if markedAfter != nil, !stale(), controller.waitingInView == 0 {
                inkedAfter = Date().timeIntervalSince(start)
                break
            }
        }
        try? await Task.sleep(for: .milliseconds(400))
        controller.view.reload()
        let after = layout()
        let moved = zip(before, after).filter { $0 != $1 }.count + abs(before.count - after.count)
        snapshot("gallery-watch.png")
        return [
            "file": path, "fileIndex": Int(file), "marked": markedAfter != nil,
            "secondsToMark": markedAfter ?? -1, "fadedWhileDrawn": staleSeen,
            "secondsToReink": inkedAfter ?? -1, "itemsCompared": before.count,
            "itemsMoved": moved, "markStill": marked(),
            "importsAgain": Int((controller.stats()?.imports ?? 0) - importsBefore),
        ]
    }

    /// Waits for the pictures in view, at most two minutes. Returns the seconds waited.
    /// What the person found trying the gallery: Esc with the Look card open closes the card
    /// first; another connected folder shown and this one again brings every file's facts and
    /// pictures back; × leaves the gallery.
    private func windowChecks(root: URL, folder: URL) async -> [String: Any] {
        guard let hooks else {
            return [:]
        }
        var out: [String: Any] = [:]
        controller.show(root: root, folder: folder)
        _ = await settle()
        // Esc, as typed, with the Look card open.
        hooks.toggleLook()
        let opened = hooks.lookOpen()
        if let escape = NSEvent.keyEvent(
            with: .keyDown,
            location: .zero,
            modifierFlags: [],
            timestamp: 0,
            windowNumber: controller.view.window?.windowNumber ?? 0,
            context: nil,
            characters: "\u{1B}",
            charactersIgnoringModifiers: "\u{1B}",
            isARepeat: false,
            keyCode: 53
        ) {
            controller.view.keyDown(with: escape)
        }
        out["escape"] = [
            "lookOpened": opened, "lookOpenAfter": hooks.lookOpen(),
            "galleryAfter": hooks.inGallery(),
        ]
        // Another root, then this one again: every file told again.
        let known = controller.factsKnown
        let other = FileManager.default.temporaryDirectory.appendingPathComponent(
            "gallery-check-other"
        )
        try? FileManager.default.createDirectory(at: other, withIntermediateDirectories: true)
        for i in 0..<2 {
            let stl =
                "solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex \(10 + i) 0 0\n"
                + "vertex 0 \(10 + i) \(10 + i)\nendloop\nendfacet\nendsolid t\n"
            try? stl.write(
                to: other.appendingPathComponent("t\(i).stl"),
                atomically: true,
                encoding: .utf8
            )
        }
        controller.show(root: other, folder: other)
        try? await Task.sleep(for: .seconds(2))
        _ = await settle()
        controller.show(root: root, folder: folder)
        let back = Date()
        while controller.factsKnown < known, Date().timeIntervalSince(back) < 60 {
            try? await Task.sleep(for: .milliseconds(200))
        }
        let waited = await settle()
        out["rootSwitch"] = [
            "factsBefore": known, "factsAfter": controller.factsKnown,
            "seconds": Date().timeIntervalSince(back), "waitedForPictures": waited,
            "waitingInView": controller.waitingInView,
        ]
        // Detail pictures: Catalogue's plate figure and Plates' covers, drawn large.
        controller.setView(GeoViewCatalogue.rawValue)
        controller.catalogue(spread: 2)
        _ = await settle()
        let detailStart = Date()
        let wanted = controller.gallery.details().count
        while controller.view.detailCount < wanted, Date().timeIntervalSince(detailStart) < 60 {
            try? await Task.sleep(for: .milliseconds(200))
        }
        snapshot("gallery-detail-catalogue.png")
        var details: [String: Any] = [
            "catalogueWanted": wanted, "catalogueShown": controller.view.detailCount,
            "catalogueSeconds": Date().timeIntervalSince(detailStart),
        ]
        controller.setView(GeoViewPlates.rawValue)
        _ = await settle()
        let platesStart = Date()
        let platesWanted = controller.gallery.details().count
        while controller.view.detailCount < platesWanted,
            Date().timeIntervalSince(platesStart) < 60
        {
            try? await Task.sleep(for: .milliseconds(200))
        }
        snapshot("gallery-detail-plates.png")
        details["platesWanted"] = platesWanted
        details["platesShown"] = controller.view.detailCount
        details["platesSeconds"] = Date().timeIntervalSince(platesStart)
        out["details"] = details
        // The masthead's ×, in the gallery.
        hooks.pressClose()
        out["close"] = ["galleryAfter": hooks.inGallery(), "viewHidden": controller.view.isHidden]
        return out
    }

    /// Where each part is drawn, by its file's path and the time it shows in the list (a part can
    /// be on a cover and in a strip): the part's own box, from its picture's note, or its measured
    /// box while it is drawn in construction lines.
    private func partsDrawn() -> [String: CGRect] {
        let gallery = controller.gallery
        var out: [String: CGRect] = [:]
        var seen: [String: Int] = [:]
        for item in gallery.items {
            let file: UInt32
            var rect = CGRect(
                x: CGFloat(item.x),
                y: CGFloat(item.y),
                width: CGFloat(item.width),
                height: CGFloat(item.height)
            )
            if item.kind == GeoItemPicture.rawValue, item.flags & GeoItemTurn.rawValue == 0 {
                file = item.file
                if let note = controller.pictureNote(at: gallery.path(of: file)), note.width > 0 {
                    let k = CGFloat(item.width) / CGFloat(note.width)
                    rect = CGRect(
                        x: CGFloat(item.x) + CGFloat(note.part.0) * k,
                        y: CGFloat(item.y) + CGFloat(note.part.1) * k,
                        width: CGFloat(note.part.2) * k,
                        height: CGFloat(note.part.3) * k
                    )
                }
            } else if item.kind == GeoItemRect.rawValue, item.flags & GeoItemDashed.rawValue != 0,
                item.target_kind == GeoTargetFile.rawValue
            {
                file = item.target
            } else {
                continue
            }
            let path = gallery.path(of: file)
            let n = seen[path, default: 0]
            seen[path] = n + 1
            out["\(path)#\(n)"] = rect
        }
        return out
    }

    /// Animations running on the gallery's layers that are not its own transitions ("gallery."):
    /// Core Animation's implicit fades, which a paint should never start.
    private func implicitAnimations() -> Int {
        func count(_ layer: CALayer) -> Int {
            let own = (layer.animationKeys() ?? []).filter { !$0.hasPrefix("gallery.") }.count
            return own + (layer.sublayers ?? []).reduce(0) { $0 + count($1) }
        }
        return controller.view.layer.map(count) ?? 0
    }

    /// The scale bars' lengths shown, in order: "1 mm", "200 mm".
    private func scaleBars() -> [String] {
        let gallery = controller.gallery
        return gallery.items.compactMap { item -> String? in
            guard item.kind == GeoItemText.rawValue else {
                return nil
            }
            let text = gallery.text(of: item)
            let units = ["mm", "m", "\u{00B5}m"]
            let parts = text.split(separator: " ")
            guard parts.count == 2, Double(parts[0].replacingOccurrences(of: ",", with: "")) != nil,
                units.contains(String(parts[1]))
            else {
                return nil
            }
            return text
        }
    }

    /// Watches the folder, just shown in Plates, settle: what each layout draws, compared with the
    /// last (nothing the gallery shows changes between layouts), until nothing in view is waiting
    /// and nothing has changed for 3 s (at most 90 s).
    private func watchSettle(root: URL, folder: URL, mode: String) async -> [String: Any] {
        let start = Date()
        final class Watch {
            var previous: [String: CGRect] = [:]
            var previousBars: [String] = []
            var moved = 0
            var went = 0
            var came = 0
            var rescaled = 0
            var changes = 0
            var layouts = 0
            var implicit = 0
            var firstChange: Double?
            var lastChange: Double?
            var firstPart: Double?
            var firstFacts: Double?
            var events: [[Any]] = []
            func note(_ t: Double, _ what: String, _ path: String) {
                if events.count < 400 {
                    events.append([(t * 100).rounded() / 100, what, path])
                }
            }
        }
        let watch = Watch()
        let look = { [self] in
            let t = Date().timeIntervalSince(start)
            watch.layouts += 1
            let now = partsDrawn()
            let bars = scaleBars()
            if watch.firstPart == nil, !now.isEmpty {
                watch.firstPart = t
            }
            var changed = false
            for (key, rect) in now {
                guard let before = watch.previous[key] else {
                    if !watch.previous.isEmpty {
                        watch.came += 1
                        changed = true
                    }
                    continue
                }
                if abs(rect.minX - before.minX) > 0.5 || abs(rect.minY - before.minY) > 0.5
                    || abs(rect.width - before.width) > 0.5
                    || abs(rect.height - before.height) > 0.5
                {
                    watch.moved += 1
                    changed = true
                    watch.note(t, "moved", key)
                }
            }
            for key in watch.previous.keys where now[key] == nil {
                watch.went += 1
                changed = true
                watch.note(t, "went", key)
            }
            if !watch.previousBars.isEmpty, bars != watch.previousBars {
                watch.rescaled += 1
                changed = true
                watch.note(t, "rescaled", bars.joined(separator: ", "))
            }
            if changed {
                watch.changes += 1
                watch.firstChange = watch.firstChange ?? t
                watch.lastChange = t
            }
            watch.previous = now
            watch.previousBars = bars
        }
        controller.view.onLaidOut = look
        look()
        while true {
            try? await Task.sleep(for: .milliseconds(50))
            let t = Date().timeIntervalSince(start)
            watch.implicit += implicitAnimations()
            if watch.firstFacts == nil, controller.factsKnown > 0 {
                watch.firstFacts = t
            }
            let quiet = t - (watch.lastChange ?? watch.firstPart ?? t)
            if (t > 1 && !controller.scanning && controller.waitingInView == 0 && quiet >= 3)
                || t > 90
            {
                break
            }
        }
        controller.view.onLaidOut = nil
        let stats = controller.stats()
        var out: [String: Any] = [
            "mode": mode, "seconds": Date().timeIntervalSince(start), "layouts": watch.layouts,
            "changes": watch.changes, "moved": watch.moved, "went": watch.went, "came": watch.came,
            "rescaled": watch.rescaled, "drawnAtEnd": watch.previous.count, "events": watch.events,
            "implicitAnimations": watch.implicit, "firstFacts": watch.firstFacts ?? -1,
            "firstPart": watch.firstPart ?? -1, "firstChange": watch.firstChange ?? -1,
            "lastChange": watch.lastChange ?? -1,
        ]
        if let stats {
            out["library"] = [
                "pictures": Int(stats.pictures), "lookups": Int(stats.lookups),
                "imports": Int(stats.imports), "structures": Int(stats.structures),
                "cancelled": Int(stats.cancelled), "failures": Int(stats.failures),
            ]
        }
        print(
            "gallery-settle \(mode): \(watch.layouts) layouts, \(watch.changes) changes, "
                + "\(watch.moved) moved, \(watch.went) went, \(watch.rescaled) rescaled, "
                + "\(watch.implicit) implicit animations; first part at "
                + String(format: "%.2f", watch.firstPart ?? -1) + " s, last change at "
                + String(format: "%.2f", watch.lastChange ?? -1) + " s"
        )
        return out
    }

    private func settle() async -> Double {
        let start = Date()
        while controller.waitingInView > 0, Date().timeIntervalSince(start) < 120 {
            try? await Task.sleep(for: .milliseconds(200))
        }
        controller.settleNow()  // what has arrived, laid out now rather than at the next step
        // A last burst of pictures to lay out and paint.
        try? await Task.sleep(for: .milliseconds(400))
        controller.view.reload()
        controller.view.repaint()
        return Date().timeIntervalSince(start)
    }

    private func layoutDump() -> [[String: Any]] {
        let gallery = controller.gallery
        return gallery.items.map { item in
            var row: [String: Any] = [
                "kind": Int(item.kind), "x": Double(item.x), "y": Double(item.y),
                "w": Double(item.width),
                "h": Double(item.height), "key": Int(item.key), "group": Int(item.group),
                "line": Double(item.line), "column": Int(item.column),
                "columnLine": Double(item.column_line),
            ]
            if item.kind == GeoItemText.rawValue {
                row["text"] = gallery.text(of: item)
                row["baseline"] = Double(item.baseline)
            }
            if item.kind == GeoItemPicture.rawValue {
                row["file"] = gallery.path(of: item.file)
            }
            return row
        }
    }

    /// The window as it shows, drawn from its layers, to look at.
    private func snapshot(_ name: String) {
        guard let root = controller.view.window?.contentView, let layer = root.layer else {
            return
        }
        let scale = controller.view.window?.backingScaleFactor ?? 2
        guard
            let context = CGContext(
                data: nil,
                width: Int(root.bounds.width * scale),
                height: Int(root.bounds.height * scale),
                bitsPerComponent: 8,
                bytesPerRow: 0,
                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
            )
        else {
            return
        }
        context.scaleBy(x: scale, y: scale)
        layer.render(in: context)
        if let image = context.makeImage() {
            let output = FileManager.default.temporaryDirectory.appendingPathComponent(name)
            try? NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])?
                .write(to: output)
        }
    }

    private func write(_ report: [String: Any], to name: String) {
        let output = FileManager.default.temporaryDirectory.appendingPathComponent(name)
        if let data = try? JSONSerialization.data(
            withJSONObject: report,
            options: [.prettyPrinted, .sortedKeys]
        ) {
            try? data.write(to: output)
            print("Wrote \(output.path)")
        }
    }
}
