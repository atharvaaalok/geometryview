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
