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
    /// The views the gallery may show: Plates and Sheet, or all five with `--gallery-views all`.
    static var views: UInt32 {
        let arguments = CommandLine.arguments
        if let at = arguments.firstIndex(of: "--gallery-views"), at + 1 < arguments.count,
            arguments[at + 1] == "all"
        {
            return 0b11111
        }
        return (1 << GeoViewPlates.rawValue) | (1 << GeoViewSheet.rawValue)
    }
    static var isLayout: Bool { CommandLine.arguments.contains("--gallery-layout") }
    /// `--gallery-poster`: the folder as a poster in each view, checked and saved as PNG.
    static var isPoster: Bool { CommandLine.arguments.contains("--gallery-poster") }
    /// Shows or puts away the poster, as the window does.
    var usePoster: ((Bool) -> Void)?
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
        // The views built so far: Catalogue comes in G8.
        let views = (0..<4).map(UInt32.init).filter { controller.gallery.views & (1 << $0) != 0 }
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
        return [
            "groupsChecked": Int(result.checked), "over": over,
            "itemsPainted": lines.filter { !$0.isNaN }.count, "maxDeviation": Double(deviation),
        ]
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

    /// The folder as a poster in Plates and in Sheet: aligned as painted, and saved as PNG, with the
    /// picture's size.
    private func posters(root: URL, folder: URL) async -> [[String: Any]] {
        controller.show(root: root, folder: folder)
        let scanStart = Date()
        while controller.scanning, Date().timeIntervalSince(scanStart) < 60 {
            try? await Task.sleep(for: .milliseconds(100))
        }
        var out: [[String: Any]] = []
        for (view, name) in [(GeoViewPlates, "plates"), (GeoViewSheet, "sheet")] {
            controller.setView(view.rawValue)
            usePoster?(true)
            try? await Task.sleep(for: .milliseconds(600))
            _ = await settle()
            var run = measure()
            run["view"] = name
            run["contentHeight"] = Double(controller.gallery.contentHeight)
            if let image = controller.view.posterImage() {
                run["pixels"] = [image.width, image.height]
                let output = FileManager.default.temporaryDirectory.appendingPathComponent(
                    "gallery-poster-\(name).png"
                )
                try? NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])?
                    .write(to: output)
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
    private func settle() async -> Double {
        let start = Date()
        while controller.waitingInView > 0, Date().timeIntervalSince(start) < 120 {
            try? await Task.sleep(for: .milliseconds(200))
        }
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
