import AppKit
import GeometryViewEngine
import ImageIO

/// A connected folder's gallery: its scan, the library service drawing what is in view first, and
/// the view painting the layout as facts and pictures arrive.
@MainActor
final class GalleryController {
    let gallery = GalleryHandle()
    let view: GalleryView
    private let library = LibraryHandle()
    private(set) var root: URL?
    /// A scan of the root is under way.
    private(set) var scanning = false

    /// What is known by path relative to the root, kept across a new scan of the same folder.
    private var facts: [String: LibraryFacts] = [:]
    private var pictures: [String: (png: String, picture: GeoGalleryPicture, style: UInt32)] = [:]
    private var failures: [String: String] = [:]
    private var entries: [String: ScanEntry] = [:]
    /// Pictures decoded, by path, so a new listing keeps them without decoding them again.
    private var decoded: [String: CGImage] = [:]
    /// Watches the root, so a file changed on disk re-inks.
    private lazy var watcher = DirectoryWatcher { [weak self] _ in self?.rootChanged() }
    private var rescanScheduled = false
    private var reloadScheduled = false
    private var wantScheduled = false
    private var picturesPending = 0
    private var styleRaw: UInt32 = 0

    /// Asked to open a file in the viewer.
    var onOpen: ((URL) -> Void)?
    /// Asked to close the gallery: Esc at its top folder.
    var onLeave: (() -> Void)?
    /// The folder in view changed, for the masthead.
    var onFolderChanged: (() -> Void)?
    /// The view in use or its controls changed, for the toolbar and the View menu.
    var onChanged: (() -> Void)?
    /// The search or what it found changed, for the search field.
    var onSearchChanged: (() -> Void)?

    /// Sheet's controls, kept from one launch to the next.
    private(set) var sheet = SheetOptions.load()
    /// Ladders' scale: one shared by every strip, kept from one launch to the next.
    private(set) var laddersShared = UserDefaults.standard.bool(forKey: "gallery.ladders.shared")

    init(views: UInt32 = (1 << GeoViewPlates.rawValue) | (1 << GeoViewSheet.rawValue)) {
        view = GalleryView(gallery: gallery)
        gallery.views = views
        gallery.setSheet(scale: sheet.scale, cell: sheet.cell, sort: sheet.sort)
        gallery.setLadders(shared: laddersShared)
        view.onAction = { [weak self] action in self?.act(action) }
        view.onViewChanged = { [weak self] in self?.scheduleWant() }
        view.onSearchChanged = { [weak self] in self?.onSearchChanged?() }
        view.onAtlasMoved = { [weak self] in self?.onChanged?() }
        library?.onEvent = { [weak self] event in self?.receive(event) }
    }

    /// Shows a view, if enabled: parts in both fly to their places.
    @discardableResult
    func setView(_ view: UInt32) -> Bool {
        guard self.view.setView(view) else {
            return false
        }
        onChanged?()
        scheduleWant()
        return true
    }

    /// Changes Sheet's scale, picture size or order.
    func setSheet(_ options: SheetOptions) {
        sheet = options
        sheet.save()
        gallery.setSheet(scale: sheet.scale, cell: sheet.cell, sort: sheet.sort)
        view.reload()
        onChanged?()
        scheduleWant()
    }

    /// Atlas: goes to a level, or a step in (1) or out (−1).
    func atlas(level: Int? = nil, step: Int = 0) {
        gallery.atlasLevel(level ?? max(0, min(2, gallery.atlasLevel + step)))
        view.repaint()
        view.reload()
        onChanged?()
        scheduleWant()
    }

    /// Catalogue: turns a spread (`step`), goes to one (`spread`), or to the index (neither).
    func catalogue(step: Int? = nil, spread: Int? = nil) {
        if let step {
            gallery.setCatalogueSpread(gallery.catalogue.spread + step)
        } else if let spread {
            gallery.setCatalogueSpread(spread)
        } else {
            gallery.catalogueIndex()
        }
        view.repaint()
        view.reload()
        onChanged?()
        scheduleWant()
    }

    /// Ladders' scale: each strip its own, or one shared.
    func setLadders(shared: Bool) {
        laddersShared = shared
        UserDefaults.standard.set(shared, forKey: "gallery.ladders.shared")
        gallery.setLadders(shared: shared)
        view.reload()
        onChanged?()
        scheduleWant()
    }

    /// Searches the folder in view; empty clears it.
    func search(_ query: String) {
        view.search(query)
    }

    /// Shows the view as a poster, dated today, or as the gallery again.
    func setPoster(_ poster: Bool) {
        let format = DateFormatter()
        format.locale = Locale(identifier: "en_US_POSIX")
        format.dateFormat = "d MMMM yyyy"
        let date = format.string(from: Date())
        gallery.setPoster(poster, date: date)
        view.scrollToTop()
        view.reload()
        scheduleWant()
    }

    var title: String { gallery.title }
    var subtitle: String { gallery.subtitle }

    /// Shows `folder` inside the connected folder `root`: at once from the last listing kept, then
    /// as a fresh scan finds it.
    func show(root: URL, folder: URL) {
        let relative = Self.relative(folder, to: root) ?? ""
        if root != self.root {
            self.root = root
            facts.removeAll()
            pictures.removeAll()
            failures.removeAll()
            decoded.removeAll()
            view.clearImages()
            watcher.watch([root.path])
            if let kept = library?.keptListing(root) {
                load(kept)
            } else {
                load([])
            }
            scanning = true
            Task { [weak self] in
                let entries = await self?.library?.scan(root)
                self?.scanning = false
                if let entries {
                    self?.rescanned(entries)
                }
            }
        }
        gallery.setFolder(relative)
        view.scrollToTop()
        view.reload()
        onFolderChanged?()
        onChanged?()
        onSearchChanged?()
        scheduleWant()
    }

    func setStyle(_ style: GeoStyle) {
        styleRaw = style.rawValue
        library?.setPicture(
            style: style,
            colorMode: GeoColorParts,
            pixels: gallery.picturePixels,
            lineScale: min(1, max(0.5, Float(gallery.picturePixels) / 2 / 320))
        )
        view.setStyle(style)
        onChanged?()
        scheduleWant()
    }

    func stats() -> GeoLibraryStats? {
        library?.stats()
    }

    /// The folders right inside the folder in view, by path relative to the root.
    func subfolders() -> [String] {
        let folder = gallery.folder
        return entries.values.filter { entry in
            guard entry.folder else {
                return false
            }
            let parent = (entry.path as NSString).deletingLastPathComponent
            return parent == folder
                && (entries.values.contains { !$0.folder && $0.path.hasPrefix(entry.path + "/") })
        }.map(\.path).sorted()
    }

    /// Pictures still to arrive for files in view, for the checks.
    var waitingInView: Int {
        gallery.wanted().filter { want in
            let path = gallery.path(of: want.file)
            return want.band <= 2 && pictures[path]?.style != styleRaw && failures[path] == nil
        }.count
    }

    // MARK: Scans

    private func load(_ list: [ScanEntry]) {
        let folder = gallery.folder
        entries = Dictionary(list.map { ($0.path, $0) }, uniquingKeysWith: { a, _ in a })
        gallery.setEntries(title: root?.lastPathComponent ?? "", entries: list)
        gallery.setFolder(folder)
        // What was known of each file is known still, its picture without decoding it again.
        view.clearImages()
        for (path, f) in facts {
            if let file = gallery.file(at: path) {
                gallery.setFacts(file, f)
            }
        }
        for (path, p) in pictures {
            if let file = gallery.file(at: path), let image = decoded[path] {
                gallery.setPicture(file, p.picture)
                view.setImage(image, for: file)
            } else if gallery.file(at: path) != nil {
                loadImage(p.png, for: path)
            }
        }
        for (path, message) in failures {
            if let file = gallery.file(at: path) {
                gallery.setFailure(file, message)
            }
        }
    }

    private func rescanned(_ list: [ScanEntry]) {
        // Unchanged, the layout stands.
        if list.count == entries.count,
            list.allSatisfy({ entries[$0.path]?.modified == $0.modified })
        {
            return
        }
        // Files drawn before whose time changed: each re-inks alone, marked until seen.
        let changed = list.filter { entry in
            !entry.folder && pictures[entry.path] != nil
                && entries[entry.path].map { $0.modified != entry.modified } == true
        }.map(\.path)
        load(list)
        for path in changed {
            if let file = gallery.file(at: path) {
                gallery.setChanged(file, true)
            }
        }
        view.reload()
        onFolderChanged?()
        scheduleWant()
    }

    /// Something under the root changed on disk: scans it again, once the burst is over.
    private func rootChanged() {
        guard !rescanScheduled, let root else {
            return
        }
        rescanScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in
            self?.rescanScheduled = false
            guard let self, self.root == root, !scanning else {
                return
            }
            scanning = true
            Task { [weak self] in
                let entries = await self?.library?.scan(root)
                self?.scanning = false
                if let entries, self?.root == root {
                    self?.rescanned(entries)
                }
            }
        }
    }

    // MARK: The library

    private func scheduleWant() {
        guard !wantScheduled else {
            return
        }
        wantScheduled = true
        DispatchQueue.main.async { [weak self] in
            self?.wantScheduled = false
            self?.sendWants()
        }
    }

    private func sendWants() {
        guard let root, let library else {
            return
        }
        let wants = gallery.wanted().map { want -> LibraryHandle.Want in
            let path = gallery.path(of: want.file)
            let entry = entries[path]
            return LibraryHandle.Want(
                path: root.appendingPathComponent(path).path,
                band: GeoLibraryBand(rawValue: want.band),
                order: want.order,
                bytes: entry?.bytes ?? 0,
                modified: entry?.modified ?? 0,
                folderColor: want.folder_color
            )
        }
        library.want(wants)
    }

    private func receive(_ event: LibraryEvent) {
        guard let root else {
            return
        }
        let relative: (String) -> String? = { path in
            Self.relative(URL(fileURLWithPath: path), to: root)
        }
        switch event {
        case .facts(let path, let f):
            guard let rel = relative(path), let file = gallery.file(at: rel) else {
                return
            }
            facts[rel] = f
            gallery.setFacts(file, f)
        case .picture(let path, let png, let picture):
            guard let rel = relative(path), gallery.file(at: rel) != nil else {
                return
            }
            // Laid out once decoded, so the old picture stays whole until the new one shows.
            pictures[rel] = (png, picture, styleRaw)
            loadImage(png, for: rel)
        case .failed(let path, let message):
            guard let rel = relative(path), let file = gallery.file(at: rel) else {
                return
            }
            failures[rel] = message
            gallery.setFailure(file, message)
        case .progress(let path, let done, let total):
            guard total > 0, let rel = relative(path), let file = gallery.file(at: rel) else {
                return
            }
            gallery.setReading(file, Float(done) / Float(total))
        }
        scheduleReload()
    }

    /// Decodes a picture off the main thread, then paints it on the file at `path`, wherever the
    /// listing then has it.
    private func loadImage(_ png: String, for path: String) {
        picturesPending += 1
        Task.detached(priority: .userInitiated) {
            let url = URL(fileURLWithPath: png) as CFURL
            let image = CGImageSourceCreateWithURL(url, nil).flatMap {
                CGImageSourceCreateImageAtIndex(
                    $0,
                    0,
                    [kCGImageSourceShouldCacheImmediately: true] as CFDictionary
                )
            }
            let sendable = UncheckedImage(image: image)
            await MainActor.run { [weak self] in
                guard let self else {
                    return
                }
                picturesPending -= 1
                // Only the picture still wanted for the path: a newer one may have come meanwhile.
                guard let wanted = pictures[path], wanted.png == png,
                    let file = gallery.file(at: path)
                else {
                    return
                }
                decoded[path] = sendable.image
                gallery.setPicture(file, wanted.picture)
                view.setImage(sendable.image, for: file)
                scheduleReload(repaint: true)
            }
        }
    }

    private var repaintScheduled = false

    /// Facts and pictures arrive in bursts: one layout for each burst.
    private func scheduleReload(repaint: Bool = false) {
        repaintScheduled = repaintScheduled || repaint
        guard !reloadScheduled else {
            return
        }
        reloadScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in
            guard let self else {
                return
            }
            reloadScheduled = false
            if repaintScheduled {
                repaintScheduled = false
                view.repaint()
            }
            view.reload()
            // What a search finds grows as names inside files arrive.
            onSearchChanged?()
        }
    }

    // MARK: Actions

    private func act(_ action: GeoGalleryAction) {
        switch action.kind {
        case GeoActionEnterFolder.rawValue:
            // A folder shows the view it last showed.
            onFolderChanged?()
            onChanged?()
            onSearchChanged?()
            scheduleWant()
        case GeoActionOpenFile.rawValue:
            if let root {
                onOpen?(root.appendingPathComponent(gallery.path(of: action.target)))
            }
        case GeoActionLeave.rawValue:
            onLeave?()
        default:
            break
        }
    }

    static func relative(_ url: URL, to root: URL) -> String? {
        let base = root.standardizedFileURL.path
        let path = url.standardizedFileURL.path
        if path == base {
            return ""
        }
        guard path.hasPrefix(base + "/") else {
            return nil
        }
        return String(path.dropFirst(base.count + 1))
    }
}

/// Sheet's controls: its scale (GeoGalleryScale), cell size in points and order (GeoGallerySort).
struct SheetOptions: Equatable {
    var scale = GeoScaleFolder.rawValue
    var cell: CGFloat = 128
    var sort = GeoSortName.rawValue

    private static let key = "gallery.sheet"

    static func load() -> SheetOptions {
        var options = SheetOptions()
        if let kept = UserDefaults.standard.dictionary(forKey: key) {
            options.scale = (kept["scale"] as? NSNumber)?.uint32Value ?? options.scale
            options.cell = CGFloat((kept["cell"] as? NSNumber)?.doubleValue ?? Double(options.cell))
            options.sort = (kept["sort"] as? NSNumber)?.uint32Value ?? options.sort
        }
        return options
    }

    func save() {
        UserDefaults.standard.set(
            [
                "scale": NSNumber(value: scale), "cell": NSNumber(value: Double(cell)),
                "sort": NSNumber(value: sort),
            ],
            forKey: Self.key
        )
    }
}

private struct UncheckedImage: @unchecked Sendable {
    let image: CGImage?
}
